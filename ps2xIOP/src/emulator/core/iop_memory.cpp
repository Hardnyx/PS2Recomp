#include "iop_memory.h"

#include "ps2x/iop/iop_host_spu2.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kDmaSpu0Madr = 0x1F8010C0u;
        constexpr uint32_t kDmaSpu1Madr = 0x1F801500u;
        constexpr uint32_t kDmaSpu0Chcr = 0x1F8010C8u;
        constexpr uint32_t kDmaSpu1Chcr = 0x1F801508u;
        constexpr uint32_t kSpu2RegBase = 0x1F900000u;
        constexpr uint32_t kSpu2RegEnd = 0x1F900800u;
        constexpr uint32_t kDmaStart = 1u << 24u;
        constexpr int kDmaSpu0Irq = 0x24;
        constexpr int kDmaSpu1Irq = 0x28;

        uint32_t alignUp(uint32_t value, uint32_t alignment)
        {
            return (value + alignment - 1u) & ~(alignment - 1u);
        }
    }

    // Host SPU2 hooks and the interrupt requests queued by iopSpu2RaiseIrq / iopSpu2DmaComplete.
    IopHostSpu2 g_hostSpu2;
    std::atomic<uint32_t> g_spu2Pending{0u};

    uint32_t takeSpu2Pending() noexcept
    {
        if (g_spu2Pending.load(std::memory_order_relaxed) == 0u)
            return 0u;
        return g_spu2Pending.exchange(0u, std::memory_order_acq_rel);
    }

    bool IopMemory::isSpu2Register(uint32_t phys) noexcept
    {
        return phys >= kSpu2RegBase && phys < kSpu2RegEnd && g_hostSpu2.read16 && g_hostSpu2.write16;
    }

    IopMemory::IopMemory()
        : m_spuRam(2u * 1024u * 1024u), m_ram(RamSize), m_owned(RamSize), m_scratch(ScratchSize)
    {
        reset();
    }

    void IopMemory::reset()
    {
        std::fill(m_ram.begin(), m_ram.end(), uint8_t{0});
        std::fill(m_owned.begin(), m_owned.end(), uint8_t{0});
        std::fill(m_scratch.begin(), m_scratch.end(), uint8_t{0});
        m_hardware.clear();
        std::fill(m_spuRam.begin(), m_spuRam.end(), uint8_t{0});
        m_spuVoices = {};
        m_spuActiveMasks = {};
        m_spuCycle = 0u;
        m_spuIrqPending = false;
        m_allocations.clear();
        m_heapCursor = HeapBase;
        m_interruptStatus = 0;
        m_interruptMask = 0;
        m_interruptControl = 1;
        m_dmaStart.reset();
    }

    uint32_t IopMemory::physicalAddress(uint32_t address) noexcept
    {
        return address & 0x1FFFFFFFu;
    }

    uint8_t IopMemory::read8(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys < RamSize)
            return m_ram[phys];
        if (phys >= ScratchBase && phys < ScratchBase + ScratchSize)
            return m_scratch[phys - ScratchBase];
        if (isSpu2Register(phys))
            return static_cast<uint8_t>(g_hostSpu2.read16(phys & ~1u, spu2Cycle()) >> ((phys & 1u) * 8u));
        const uint32_t value = readHardware32(phys & ~3u);
        return static_cast<uint8_t>(value >> ((phys & 3u) * 8u));
    }

    uint16_t IopMemory::read16(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys + 1u < RamSize)
        {
            uint16_t value;
            std::memcpy(&value, m_ram.data() + phys, sizeof(value));
            return value;
        }
        if ((phys & 1u) == 0u && isSpu2Register(phys))
            return g_hostSpu2.read16(phys, spu2Cycle());
        return static_cast<uint16_t>(read8(address) | (static_cast<uint16_t>(read8(address + 1u)) << 8u));
    }

    uint32_t IopMemory::read32(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if ((phys & 3u) == 0u && phys + 3u < RamSize)
        {
            uint32_t value;
            std::memcpy(&value, m_ram.data() + phys, sizeof(value));
            return value;
        }
        if ((phys & 3u) == 0u && phys >= ScratchBase && phys + 3u < ScratchBase + ScratchSize)
        {
            uint32_t value;
            std::memcpy(&value, m_scratch.data() + (phys - ScratchBase), sizeof(value));
            return value;
        }
        if ((phys & 3u) == 0u && isSpu2Register(phys))
        {
            const uint32_t lo = g_hostSpu2.read16(phys, spu2Cycle());
            return lo | (static_cast<uint32_t>(g_hostSpu2.read16(phys + 2u, spu2Cycle())) << 16u);
        }
        if ((phys & 3u) == 0u && isHardwareAddress(phys))
            return readHardware32(phys);

        return static_cast<uint32_t>(read8(address)) |
               (static_cast<uint32_t>(read8(address + 1u)) << 8u) |
               (static_cast<uint32_t>(read8(address + 2u)) << 16u) |
               (static_cast<uint32_t>(read8(address + 3u)) << 24u);
    }

    void IopMemory::write8(uint32_t address, uint8_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys < RamSize)
        {
            m_ram[phys] = value;
            markOwned(phys, sizeof(value));
            return;
        }
        if (phys >= ScratchBase && phys < ScratchBase + ScratchSize)
        {
            m_scratch[phys - ScratchBase] = value;
            return;
        }
        if (isSpu2Register(phys))
        {
            const uint32_t reg = phys & ~1u;
            const uint32_t shift = (phys & 1u) * 8u;
            const uint32_t current = g_hostSpu2.read16(reg, spu2Cycle());
            const uint32_t merged = (current & ~(0xFFu << shift)) | (static_cast<uint32_t>(value) << shift);
            g_hostSpu2.write16(reg, static_cast<uint16_t>(merged), spu2Cycle());
            return;
        }
        const uint32_t aligned = phys & ~3u;
        uint32_t current = readHardware32(aligned);
        const uint32_t shift = (phys & 3u) * 8u;
        current = (current & ~(0xFFu << shift)) | (static_cast<uint32_t>(value) << shift);
        writeHardware32(aligned, current);
    }

    void IopMemory::write16(uint32_t address, uint16_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys + 1u < RamSize)
        {
            std::memcpy(m_ram.data() + phys, &value, sizeof(value));
            markOwned(phys, sizeof(value));
            return;
        }
        if ((phys & 1u) == 0u && isSpu2Register(phys))
        {
            g_hostSpu2.write16(phys, value, spu2Cycle());
            return;
        }
        if (isHardwareAddress(phys) && (phys & 1u) == 0u)
        {
            const uint32_t aligned = phys & ~3u;
            const uint32_t shift = (phys & 2u) * 8u;
            const uint32_t current = readHardware32(aligned);
            writeHardware32(aligned, (current & ~(0xFFFFu << shift)) | (static_cast<uint32_t>(value) << shift));
            return;
        }
        write8(address, static_cast<uint8_t>(value));
        write8(address + 1u, static_cast<uint8_t>(value >> 8u));
    }

    void IopMemory::write32(uint32_t address, uint32_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if ((phys & 3u) == 0u && phys + 3u < RamSize)
        {
            std::memcpy(m_ram.data() + phys, &value, sizeof(value));
            markOwned(phys, sizeof(value));
            return;
        }
        if ((phys & 3u) == 0u && phys >= ScratchBase && phys + 3u < ScratchBase + ScratchSize)
        {
            std::memcpy(m_scratch.data() + (phys - ScratchBase), &value, sizeof(value));
            return;
        }
        if ((phys & 3u) == 0u && isSpu2Register(phys))
        {
            g_hostSpu2.write16(phys, static_cast<uint16_t>(value), spu2Cycle());
            g_hostSpu2.write16(phys + 2u, static_cast<uint16_t>(value >> 16u), spu2Cycle());
            return;
        }
        if ((phys & 3u) == 0u)
        {
            writeHardware32(phys, value);
            return;
        }
        write8(address, static_cast<uint8_t>(value));
        write8(address + 1u, static_cast<uint8_t>(value >> 8u));
        write8(address + 2u, static_cast<uint8_t>(value >> 16u));
        write8(address + 3u, static_cast<uint8_t>(value >> 24u));
    }

    bool IopMemory::readRam(uint32_t address, void *destination, size_t size) const
    {
        const uint32_t phys = physicalAddress(address);
        if ((!destination && size != 0u) || phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
            std::memcpy(destination, m_ram.data() + phys, size);
        return true;
    }

    bool IopMemory::writeRam(uint32_t address, const void *source, size_t size)
    {
        const uint32_t phys = physicalAddress(address);
        if ((!source && size != 0u) || phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
        {
            std::memcpy(m_ram.data() + phys, source, size);
            markOwned(phys, size);
        }
        return true;
    }

    bool IopMemory::zeroRam(uint32_t address, size_t size)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
        {
            std::memset(m_ram.data() + phys, 0, size);
            markOwned(phys, size);
        }
        return true;
    }

    bool IopMemory::ownsRamRange(uint32_t address, size_t size) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys > RamSize || size > RamSize - phys)
            return false;
        return std::all_of(m_owned.begin() + phys, m_owned.begin() + phys + size,
                           [](uint8_t value)
                           { return value != 0u; });
    }

    void IopMemory::markOwned(uint32_t address, size_t size)
    {
        if (address > RamSize || size > RamSize - address)
            return;
        std::fill(m_owned.begin() + address, m_owned.begin() + address + size, uint8_t{1});
    }

    bool IopMemory::isHardwareAddress(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        return (phys >= HardwareBase && phys < HardwareEnd) ||
               (phys >= Spu2Base && phys < Spu2End) ||
               (phys >= SifBase && phys < SifEnd);
    }

    uint32_t IopMemory::readHardware32(uint32_t address) const
    {
        if ((address == kDmaSpu0Madr || address == kDmaSpu1Madr) && g_hostSpu2.dmaStart && g_hostSpu2.dmaMadr)
        {
            const auto chcr = m_hardware.find(address + 8u);
            if (chcr != m_hardware.end() && (chcr->second & kDmaStart) != 0u)
                return g_hostSpu2.dmaMadr(address == kDmaSpu1Madr ? 1 : 0);
        }
        const auto value = m_hardware.find(address);
        if (value != m_hardware.end())
            return value->second;
        switch (address)
        {
        case 0x1F801070u:
            return m_interruptStatus;
        case 0x1F801074u:
            return m_interruptMask;
        case 0x1F801078u:
            return m_interruptControl;
        default:
            return 0u;
        }
    }

    void IopMemory::writeHardware32(uint32_t address, uint32_t value)
    {
        switch (address)
        {
        case 0x1F801070u:
            m_interruptStatus &= value;
            return;
        case 0x1F801074u:
            m_interruptMask = value;
            return;
        case 0x1F801078u:
            m_interruptControl = value & 1u;
            return;
        default:
            break;
        }

        m_hardware[address] = value;
        if (address >= Spu2Base && address < Spu2Base + 0x800u && !isSpu2Register(address))
            writeSpu2Control(address, value);
        if ((address != kDmaSpu0Chcr && address != kDmaSpu1Chcr) || (value & kDmaStart) == 0u)
            return;
        if (g_hostSpu2.dmaStart)
        {
            startSpu2Dma(address, value); // CHCR.TR stays set until the host completes the transfer
            return;
        }

        const bool secondCore = address == kDmaSpu1Chcr;
        m_hardware[address] = value & ~kDmaStart;

        const uint32_t statusAddress = 0x1F900344u + (secondCore ? 0x400u : 0u);
        const uint32_t alignedStatus = statusAddress & ~3u;
        const uint32_t shift = (statusAddress & 2u) * 8u;
        uint32_t status = 0u;
        if (const auto current = m_hardware.find(alignedStatus); current != m_hardware.end())
            status = current->second;
        status |= 0x80u << shift;
        m_hardware[alignedStatus] = status;

        const uint32_t blockControlAddress = address - sizeof(uint32_t);
        uint32_t blockControl = 0u;
        if (const auto current = m_hardware.find(blockControlAddress); current != m_hardware.end())
            blockControl = current->second;
        const uint32_t wordsPerBlock = std::max<uint32_t>(blockControl & 0xFFFFu, 1u);
        const uint32_t blockCount = std::max<uint32_t>(blockControl >> 16u, 1u);
        const uint64_t transferWords = static_cast<uint64_t>(wordsPerBlock) * blockCount;
        // DMA BCR counts 32-bit words; SPU2 RAM is addressed in 16-bit halfwords.
        transferSpu2Dma(address, value, transferWords * 2u);
        m_dmaStart = DmaStart{
            secondCore ? kDmaSpu1Irq : kDmaSpu0Irq,
            std::max<uint64_t>(transferWords * 2u, 64u),
        };
    }

    // TEKKEN_IOP_SPU2_STREAM_V1: this models transport and voice addresses, not an audio mixer.
    uint32_t IopMemory::spu2Address(uint32_t reg) const
    {
        return ((static_cast<uint32_t>(read16(reg)) & 0xFu) << 16u) | read16(reg + 2u);
    }

    void IopMemory::setSpu2Address(uint32_t reg, uint32_t word)
    {
        word &= 0xFFFFFu;
        m_hardware[reg] = ((word & 0xFFFFu) << 16u) | (word >> 16u);
    }

    void IopMemory::checkSpu2Irq(uint32_t word)
    {
        for (uint32_t core = 0u; core < 2u; ++core)
        {
            const uint32_t base = Spu2Base + core * 0x400u;
            if ((read16(base + 0x19Au) & 0x40u) == 0u ||
                (read16(base + 0x344u) & 0x40u) != 0u ||
                spu2Address(base + 0x19Cu) != word)
                continue;
            m_hardware[base + 0x344u] = readHardware32(base + 0x344u) | 0x40u;
            m_hardware[Spu2Base + 0x7C0u] = readHardware32(Spu2Base + 0x7C0u) | ((4u << core) << 16u);
            m_spuIrqPending = true;


        }
    }

    void IopMemory::writeSpu2Control(uint32_t address, uint32_t value)
    {
        const uint32_t core = (address - Spu2Base) / 0x400u;
        const uint32_t base = Spu2Base + core * 0x400u;
        const uint32_t reg = address - base;
        if (reg == 0x1ACu)
        {
            const uint32_t word = spu2Address(base + 0x1A8u);
            m_spuRam[word * 2u] = static_cast<uint8_t>(value);
            m_spuRam[word * 2u + 1u] = static_cast<uint8_t>(value >> 8u);
            checkSpu2Irq(word);
            setSpu2Address(base + 0x1A8u, word + 1u);
        }
        if (reg == 0x198u)
        {
            const uint32_t attr = value >> 16u;
            if ((attr & 0x40u) == 0u)
            {
                m_hardware[base + 0x344u] = readHardware32(base + 0x344u) & ~0x40u;
                m_hardware[Spu2Base + 0x7C0u] = readHardware32(Spu2Base + 0x7C0u) & ~((4u << core) << 16u);
                if ((readHardware32(Spu2Base + 0x7C0u) & 0x000C0000u) == 0u)
                    m_spuIrqPending = false;
            }
            if ((attr & 0x8000u) == 0u)
            {
                m_spuActiveMasks[core] = 0u;
                for (auto &voice : m_spuVoices[core])
                    voice.active = false;
            }
        }
        if (reg == 0x1A0u || reg == 0x1A4u)
        {
            for (uint32_t i = 0u; i < 24u; ++i)
            {
                if ((value & (1u << i)) == 0u)
                    continue;
                auto &voice = m_spuVoices[core][i];
                voice = {};
                voice.active = reg == 0x1A0u;
                if (voice.active) m_spuActiveMasks[core] |= 1u << i;
                else m_spuActiveMasks[core] &= ~(1u << i);
                voice.wordAddress = spu2Address(base + 0x1C0u + i * 12u) & ~7u;
                m_hardware[base + 0x340u] = readHardware32(base + 0x340u) & ~(1u << i);
                // ENVX is a minimal activity indication; ADSR/mixing remain unimplemented.
                const uint32_t envReg = base + i * 16u + 8u;
                m_hardware[envReg] = (readHardware32(envReg) & 0xFFFFu) | (voice.active ? 0x7FFF0000u : 0u);
            }
            // KON/KOFF are commands rather than persistent readable voice masks.
            m_hardware[address] = 0u;
        }
    }

    void IopMemory::transferSpu2Dma(uint32_t address, uint32_t value, uint64_t words)
    {
        const uint32_t core = address == kDmaSpu1Chcr ? 1u : 0u;
        const uint32_t base = Spu2Base + core * 0x400u;
        const uint32_t source = readHardware32(address - 8u) & 0x1FFFFCu;
        uint32_t sound = spu2Address(base + 0x1A8u);
        const uint64_t availableWords = (RamSize - source) / 2u;
        const uint64_t count = std::min(words, availableWords);
        for (uint64_t i = 0u; i < count; ++i)
        {
            const uint32_t byte = (sound & 0xFFFFFu) * 2u;
            const uint32_t ram = source + static_cast<uint32_t>(i * 2u);
            checkSpu2Irq(sound);
            if ((value & 1u) != 0u)
            {
                m_spuRam[byte] = m_ram[ram];
                m_spuRam[byte + 1u] = m_ram[ram + 1u];
            }
            else
            {
                m_ram[ram] = m_spuRam[byte];
                m_ram[ram + 1u] = m_spuRam[byte + 1u];
            }
            sound = (sound + 1u) & 0xFFFFFu;
        }
        if ((value & 1u) == 0u)
            markOwned(source, static_cast<size_t>(count * 2u));
        setSpu2Address(base + 0x1A8u, sound);
    }

    uint64_t IopMemory::nextSpu2EventCycle(uint64_t limit) const
    {
        return (m_spuActiveMasks[0] | m_spuActiveMasks[1]) != 0u
            ? std::min(limit, m_spuCycle + 768u) : limit;
    }

    void IopMemory::advanceSpu2(uint64_t cycle)
    {
        if ((m_spuActiveMasks[0] | m_spuActiveMasks[1]) == 0u)
        {
            m_spuCycle = cycle;
            return;
        }
        while (cycle >= m_spuCycle && cycle - m_spuCycle >= 768u)
        {
            m_spuCycle += 768u; // 36.864 MHz IOP clock / 48 kHz SPU2 sample rate.
            for (uint32_t core = 0u; core < 2u; ++core)
            {
                const uint32_t base = Spu2Base + core * 0x400u;
                if ((read16(base + 0x19Au) & 0x8000u) == 0u)
                    continue;
                for (uint32_t i = 0u; i < 24u; ++i)
                {
                    auto &voice = m_spuVoices[core][i];
                    if (!voice.active)
                        continue;
                    const uint32_t pitch = read16(base + i * 16u + 4u) & 0x3FFFu;
                    if (pitch == 0u)
                        continue;
                    if (voice.fetch)
                    {
                        voice.flags = m_spuRam[(voice.wordAddress * 2u + 1u) & 0x1FFFFFu];
                        for (uint32_t word = 0u; word < 8u; ++word)
                            checkSpu2Irq((voice.wordAddress + word) & 0xFFFFFu);
                        if ((voice.flags & 4u) != 0u)
                            setSpu2Address(base + 0x1C4u + i * 12u, voice.wordAddress);
                        voice.fetch = false;
                        setSpu2Address(base + 0x1C8u + i * 12u, voice.wordAddress + 8u);
                    }
                    voice.samplePhase += pitch;
                    if (voice.samplePhase < 28u * 4096u)
                        continue;
                    voice.samplePhase -= 28u * 4096u;
                    voice.wordAddress = (voice.wordAddress + 8u) & 0xFFFFFu;
                    voice.fetch = true;
                    if ((voice.flags & 1u) == 0u)
                        continue;
                    m_hardware[base + 0x340u] = readHardware32(base + 0x340u) | (1u << i);
                    if ((voice.flags & 2u) != 0u)
                        voice.wordAddress = spu2Address(base + 0x1C4u + i * 12u) & ~7u;
                    else
                    {
                        voice.active = false;
                        m_spuActiveMasks[core] &= ~(1u << i);
                        const uint32_t envReg = base + i * 16u + 8u;
                        m_hardware[envReg] = readHardware32(envReg) & 0xFFFFu;
                    }
                }
            }
        }
    }

    void IopMemory::startSpu2Dma(uint32_t chcrAddress, uint32_t chcr)
    {
        const int core = chcrAddress == kDmaSpu1Chcr ? 1 : 0;
        const uint32_t base = chcrAddress - 8u;
        const auto madrIt = m_hardware.find(base);
        const auto bcrIt = m_hardware.find(base + 4u);
        const uint32_t madr = (madrIt != m_hardware.end() ? madrIt->second : 0u) & 0x00FFFFFFu;
        const uint32_t bcr = bcrIt != m_hardware.end() ? bcrIt->second : 0u;
        // As PCSX2's psxDmaGeneric: words = block count * block size
        const uint64_t words = static_cast<uint64_t>(bcr >> 16u) * (bcr & 0xFFFFu);
        const uint32_t phys = madr & (RamSize - 1u) & ~1u;
        const uint64_t bytes = std::min<uint64_t>(words * 4u, RamSize - phys);
        m_spu2DmaEnd[core] = madr + static_cast<uint32_t>(words * 4u);
        g_hostSpu2.dmaStart(core,
                            reinterpret_cast<uint16_t *>(m_ram.data() + phys),
                            madr,
                            static_cast<uint32_t>(bytes / 2u),
                            (chcr & 1u) != 0u,
                            spu2Cycle());
    }

    bool IopMemory::completeSpu2Dma(int core)
    {
        const uint32_t chcrAddress = core ? kDmaSpu1Chcr : kDmaSpu0Chcr;
        const auto chcr = m_hardware.find(chcrAddress);
        if (chcr == m_hardware.end() || (chcr->second & kDmaStart) == 0u)
            return false; // stopped by the guest (CHCR.TR cleared) before the SPU2 finished
        chcr->second &= ~kDmaStart;
        m_hardware[chcrAddress - 8u] = g_hostSpu2.dmaMadr ? g_hostSpu2.dmaMadr(core) : m_spu2DmaEnd[core];
        return true;
    }

    std::optional<IopMemory::DmaStart> IopMemory::takeDmaStart() noexcept
    {
        std::optional<DmaStart> result = m_dmaStart;
        m_dmaStart.reset();
        return result;
    }

    uint32_t IopMemory::allocate(uint32_t size, uint32_t alignment, std::optional<uint32_t> fixed)
    {
        size = alignUp(std::max(size, 1u), 16u);
        alignment = std::max<uint32_t>(alignment, 4u);
        if (fixed)
        {
            const uint32_t address = *fixed;
            if (address < HeapBase || address + size > HeapLimit)
                return 0u;
            for (const auto &block : m_allocations)
                if (address < block.address + block.size && block.address < address + size)
                    return 0u;
            m_allocations.push_back({address, size});
            markOwned(address, size);
            return address;
        }

        uint32_t candidate = alignUp(m_heapCursor, alignment);
        for (;;)
        {
            bool overlap = false;
            for (const auto &block : m_allocations)
            {
                if (candidate < block.address + block.size && block.address < candidate + size)
                {
                    candidate = alignUp(block.address + block.size, alignment);
                    overlap = true;
                    break;
                }
            }
            if (!overlap)
                break;
        }
        if (candidate > HeapLimit || size > HeapLimit - candidate)
            return 0u;
        m_allocations.push_back({candidate, size});
        markOwned(candidate, size);
        m_heapCursor = std::max(m_heapCursor, candidate + size);
        return candidate;
    }

    bool IopMemory::freeAllocation(uint32_t address)
    {
        const auto block = std::find_if(m_allocations.begin(), m_allocations.end(),
                                        [&](const Allocation &candidate)
                                        { return candidate.address == address; });
        if (block == m_allocations.end())
            return false;
        std::fill(m_owned.begin() + block->address,
                  m_owned.begin() + block->address + block->size,
                  uint8_t{0});
        m_allocations.erase(block);
        return true;
    }

    uint32_t IopMemory::maxFreeMemory() const
    {
        return m_heapCursor < HeapLimit ? HeapLimit - m_heapCursor : 0u;
    }

    std::optional<IopMemory::Allocation> IopMemory::allocationContaining(uint32_t address) const
    {
        const auto block = std::find_if(m_allocations.begin(), m_allocations.end(),
                                        [&](const Allocation &candidate)
                                        {
                                            return address >= candidate.address &&
                                                   address < candidate.address + candidate.size;
                                        });
        if (block == m_allocations.end())
            return std::nullopt;
        return *block;
    }

    std::string IopMemory::readString(uint32_t address, size_t limit) const
    {
        std::string result;
        result.reserve(std::min<size_t>(limit, 64u));
        for (size_t i = 0; i < limit; ++i)
        {
            const char ch = static_cast<char>(read8(address + static_cast<uint32_t>(i)));
            if (ch == '\0')
                break;
            result.push_back(ch);
        }
        return result;
    }
}

namespace ps2x::iop
{
    void iopSetHostSpu2(const IopHostSpu2 &hooks)
    {
        detail::g_hostSpu2 = hooks;
    }

    const IopHostSpu2 &iopHostSpu2()
    {
        return detail::g_hostSpu2;
    }

    void iopSpu2RaiseIrq()
    {
        detail::g_spu2Pending.fetch_or(1u, std::memory_order_acq_rel);
    }

    void iopSpu2DmaComplete(int core)
    {
        detail::g_spu2Pending.fetch_or(core ? 4u : 2u, std::memory_order_acq_rel);
    }
}
