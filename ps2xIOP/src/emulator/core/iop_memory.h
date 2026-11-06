#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps2x::iop::detail
{
    class IopMemory
    {
    public:
        static constexpr uint32_t RamSize = 2u * 1024u * 1024u;
        static constexpr uint32_t ScratchBase = 0x1F800000u;
        static constexpr uint32_t ScratchSize = 0x400u;
        static constexpr uint32_t HardwareBase = 0x1F801000u;
        static constexpr uint32_t HardwareEnd = 0x1F900000u;
        static constexpr uint32_t Spu2Base = 0x1F900000u;
        static constexpr uint32_t Spu2End = 0x1FA00000u;
        static constexpr uint32_t SifBase = 0x1D000000u;
        static constexpr uint32_t SifEnd = 0x1D001000u;
        static constexpr uint32_t HeapBase = 0x00120000u;
        static constexpr uint32_t HeapLimit = 0x001F0000u;

        struct Allocation
        {
            uint32_t address = 0;
            uint32_t size = 0;
        };

        struct DmaStart
        {
            int irq = 0;
            uint64_t delayCycles = 0;
        };

        IopMemory();

        void reset();

        [[nodiscard]] uint8_t read8(uint32_t address) const;
        [[nodiscard]] uint16_t read16(uint32_t address) const;
        [[nodiscard]] uint32_t read32(uint32_t address) const;
        void write8(uint32_t address, uint8_t value);
        void write16(uint32_t address, uint16_t value);
        void write32(uint32_t address, uint32_t value);

        [[nodiscard]] bool readRam(uint32_t address, void *destination, size_t size) const;
        [[nodiscard]] bool writeRam(uint32_t address, const void *source, size_t size);
        [[nodiscard]] bool zeroRam(uint32_t address, size_t size);
        [[nodiscard]] bool ownsRamRange(uint32_t address, size_t size) const;
        [[nodiscard]] bool isHardwareAddress(uint32_t address) const;
        [[nodiscard]] std::string readString(uint32_t address, size_t limit = 1024u) const;

        [[nodiscard]] uint32_t allocate(uint32_t size, uint32_t alignment = 16u, std::optional<uint32_t> fixed = std::nullopt);
        [[nodiscard]] bool freeAllocation(uint32_t address);
        [[nodiscard]] uint32_t maxFreeMemory() const;
        [[nodiscard]] std::optional<Allocation> allocationContaining(uint32_t address) const;

        [[nodiscard]] uint32_t interruptStatus() const noexcept { return m_interruptStatus; }
        [[nodiscard]] uint32_t interruptMask() const noexcept { return m_interruptMask; }
        [[nodiscard]] uint32_t interruptControl() const noexcept { return m_interruptControl; }
        void setInterruptStatus(uint32_t value) noexcept { m_interruptStatus = value; }
        void setInterruptMask(uint32_t value) noexcept { m_interruptMask = value; }
        void setInterruptControl(uint32_t value) noexcept { m_interruptControl = value & 1u; }

        // TEKKEN_IOP_SPU2_STREAM_V1: guest-driven sound RAM, voice progress and IRQs.
        void advanceSpu2(uint64_t cycle);
        [[nodiscard]] uint64_t nextSpu2EventCycle(uint64_t limit) const;
        [[nodiscard]] bool spu2InterruptPending() const noexcept { return m_spuIrqPending; }
        void acknowledgeSpu2Interrupt() noexcept { m_spuIrqPending = false; }
        [[nodiscard]] std::optional<DmaStart> takeDmaStart() noexcept;

        // Host SPU2 (ps2x/iop/iop_host_spu2.h): the IOP cycle the hooks see is *epoch + *cycles.
        void setCycleSource(const uint64_t *cycles, const uint64_t *epoch) noexcept
        {
            m_cycles = cycles;
            m_cycleEpoch = epoch;
        }
        [[nodiscard]] uint64_t spu2Cycle() const noexcept
        {
            return (m_cycles ? *m_cycles : 0u) + (m_cycleEpoch ? *m_cycleEpoch : 0u);
        }
        // DMA ch4 (core 0) / ch7 (core 1) finished: clears CHCR.TR and moves MADR to the end of the transfer.
        // Returns false if the guest had already stopped the channel (no interrupt then).
        [[nodiscard]] bool completeSpu2Dma(int core);
        [[nodiscard]] std::span<const uint8_t> ram() const noexcept { return m_ram; }

        [[nodiscard]] static uint32_t physicalAddress(uint32_t address) noexcept;

    private:
        [[nodiscard]] uint32_t readHardware32(uint32_t address) const;
        void writeHardware32(uint32_t address, uint32_t value);
        void markOwned(uint32_t address, size_t size);
        [[nodiscard]] static bool isSpu2Register(uint32_t phys) noexcept;
        void startSpu2Dma(uint32_t chcrAddress, uint32_t chcr);

        struct SpuVoice
        {
            uint32_t wordAddress = 0u;
            uint32_t samplePhase = 0u;
            uint8_t flags = 0u;
            bool active = false;
            bool fetch = true;
        };
        void checkSpu2Irq(uint32_t wordAddress);
        void transferSpu2Dma(uint32_t address, uint32_t value, uint64_t words);
        void writeSpu2Control(uint32_t address, uint32_t value);
        [[nodiscard]] uint32_t spu2Address(uint32_t registerAddress) const;
        void setSpu2Address(uint32_t registerAddress, uint32_t wordAddress);
        std::vector<uint8_t> m_spuRam;
        std::array<std::array<SpuVoice, 24>, 2> m_spuVoices{};
        std::array<uint32_t, 2> m_spuActiveMasks{};
        uint64_t m_spuCycle = 0u;
        bool m_spuIrqPending = false;
        std::vector<uint8_t> m_ram;
        std::vector<uint8_t> m_owned;
        std::vector<uint8_t> m_scratch;
        std::unordered_map<uint32_t, uint32_t> m_hardware;
        std::vector<Allocation> m_allocations;
        uint32_t m_heapCursor = HeapBase;
        uint32_t m_interruptStatus = 0;
        uint32_t m_interruptMask = 0;
        uint32_t m_interruptControl = 1;
        std::optional<DmaStart> m_dmaStart;
        const uint64_t *m_cycles = nullptr;
        const uint64_t *m_cycleEpoch = nullptr;
        uint32_t m_spu2DmaEnd[2] = {};
    };

    // Host SPU2 interrupt requests queued by iopSpu2RaiseIrq / iopSpu2DmaComplete: bit 0 = SPU2 IRQ (line 9),
    // bit 1 = DMA ch4 end, bit 2 = DMA ch7 end. Taken (and cleared) by the emulator.
    [[nodiscard]] uint32_t takeSpu2Pending() noexcept;
}
