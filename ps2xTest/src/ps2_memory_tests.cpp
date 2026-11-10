#include "MiniTest.h"
#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "Stubs/DMA.h"
#include "Stubs/GS.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
    uint32_t makeVifCmd(uint8_t opcode, uint8_t num, uint16_t imm)
    {
        return (static_cast<uint32_t>(opcode) << 24) |
               (static_cast<uint32_t>(num) << 16) |
               static_cast<uint32_t>(imm);
    }

    void setRegU32(R5900Context &ctx, int reg, uint32_t value)
    {
        ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(value));
    }

    void appendU32(std::vector<uint8_t> &dst, uint32_t value)
    {
        const size_t pos = dst.size();
        dst.resize(pos + sizeof(uint32_t));
        std::memcpy(dst.data() + pos, &value, sizeof(uint32_t));
    }

    void appendU64(std::vector<uint8_t> &dst, uint64_t value)
    {
        const size_t pos = dst.size();
        dst.resize(pos + sizeof(uint64_t));
        std::memcpy(dst.data() + pos, &value, sizeof(uint64_t));
    }

    uint64_t makeDmaTag(uint16_t qwc, uint8_t id, uint32_t addr, bool irq = false)
    {
        return static_cast<uint64_t>(qwc) |
               (static_cast<uint64_t>(id & 0x7u) << 28) |
               (irq ? (1ull << 31) : 0ull) |
               (static_cast<uint64_t>(addr & 0x7FFFFFFFu) << 32);
    }

    void writeDmaTag(uint8_t *rdram, uint32_t tagAddr, uint64_t tagLo)
    {
        std::memset(rdram + tagAddr, 0, 16);
        std::memcpy(rdram + tagAddr, &tagLo, sizeof(tagLo));
    }

    void writeU64(uint8_t *rdram, uint32_t addr, uint64_t value)
    {
        std::memcpy(rdram + addr, &value, sizeof(value));
    }

    uint64_t makeGifTag(uint16_t nloop, uint8_t flg, uint8_t nreg, bool eop);

    uint64_t makeBitbltbuf(uint32_t dbp, uint32_t dbw, uint32_t dpsm)
    {
        return (static_cast<uint64_t>(dbp & 0x3FFFu) << 32) |
               (static_cast<uint64_t>(dbw & 0x3Fu) << 48) |
               (static_cast<uint64_t>(dpsm & 0x3Fu) << 56);
    }

    uint32_t writeGifAd(uint8_t *rdram, uint32_t addr, uint64_t value, uint64_t reg)
    {
        writeU64(rdram, addr + 0u, value);
        writeU64(rdram, addr + 8u, reg);
        return addr + 16u;
    }

    uint32_t writeTextureUploadSetup(uint8_t *rdram, uint32_t addr, uint32_t dbp, uint32_t dpsm)
    {
        writeDmaTag(rdram, addr, makeDmaTag(5u, 1u, 0u, false)); // CNT: setup tag + four A+D writes.
        addr += 16u;
        writeU64(rdram, addr + 0u, makeGifTag(4u, GIF_FMT_PACKED, 1u, false));
        writeU64(rdram, addr + 8u, 0x0Eull); // GIF PACKED A+D descriptor.
        addr += 16u;
        addr = writeGifAd(rdram, addr, makeBitbltbuf(dbp, 1u, dpsm), GS_REG_BITBLTBUF);
        addr = writeGifAd(rdram, addr, 0ull, GS_REG_TRXPOS);
        addr = writeGifAd(rdram, addr, (16ull << 0) | (16ull << 32), GS_REG_TRXREG);
        addr = writeGifAd(rdram, addr, 0ull, GS_REG_TRXDIR);
        return addr;
    }

    uint32_t writeTextureImageRef(uint8_t *rdram, uint32_t addr, uint32_t qwc, uint32_t dataAddr)
    {
        writeDmaTag(rdram, addr, makeDmaTag(1u, 1u, 0u, false)); // CNT: GIF IMAGE tag.
        addr += 16u;
        writeU64(rdram, addr + 0u, makeGifTag(static_cast<uint16_t>(qwc), GIF_FMT_IMAGE, 0u, false));
        writeU64(rdram, addr + 8u, 0ull);
        addr += 16u;
        writeDmaTag(rdram, addr, makeDmaTag(static_cast<uint16_t>(qwc), 3u, dataAddr, false)); // REF: image payload.
        return addr + 16u;
    }

    uint64_t makeGifTag(uint16_t nloop, uint8_t flg, uint8_t nreg, bool eop = true)
    {
        uint64_t tag = static_cast<uint64_t>(nloop & 0x7FFFu);
        if (eop)
            tag |= (1ull << 15);
        tag |= (static_cast<uint64_t>(flg & 0x3u) << 58);
        tag |= (static_cast<uint64_t>(nreg & 0xFu) << 60);
        return tag;
    }

    uint64_t makeGifTagPrim(uint16_t nloop, uint16_t prim, uint8_t flg, uint8_t nreg, bool eop = true, bool pre = true)
    {
        uint64_t tag = makeGifTag(nloop, flg, nreg, eop);
        if (pre)
            tag |= (1ull << 46);
        tag |= (static_cast<uint64_t>(prim & 0x7FFu) << 47);
        return tag;
    }

    uint64_t makeGsFrame(uint32_t fbp, uint32_t fbw, uint32_t psm, uint32_t mask = 0u)
    {
        return static_cast<uint64_t>(fbp & 0x1FFu) |
               (static_cast<uint64_t>(fbw & 0x3Fu) << 16u) |
               (static_cast<uint64_t>(psm & 0x3Fu) << 24u) |
               (static_cast<uint64_t>(mask) << 32u);
    }

    uint64_t makeGsScissor(uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1)
    {
        return static_cast<uint64_t>(x0 & 0x7FFu) |
               (static_cast<uint64_t>(x1 & 0x7FFu) << 16u) |
               (static_cast<uint64_t>(y0 & 0x7FFu) << 32u) |
               (static_cast<uint64_t>(y1 & 0x7FFu) << 48u);
    }

    void appendPackedRgbaq(std::vector<uint8_t> &packet, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        appendU64(packet, static_cast<uint64_t>(r) | (static_cast<uint64_t>(g) << 32u));
        appendU64(packet, static_cast<uint64_t>(b) | (static_cast<uint64_t>(a) << 32u));
    }

    void appendPackedXyzf2(std::vector<uint8_t> &packet, uint32_t x, uint32_t y, uint32_t z)
    {
        appendU64(packet, static_cast<uint64_t>(x & 0xFFFFu) | (static_cast<uint64_t>(y & 0xFFFFu) << 32u));
        appendU64(packet, static_cast<uint64_t>(z & 0xFFFFFFu) << 4u);
    }

    void appendPackedUv(std::vector<uint8_t> &packet, uint32_t u, uint32_t v)
    {
        appendU64(packet, static_cast<uint64_t>(u & 0x3FFFu) | (static_cast<uint64_t>(v & 0x3FFFu) << 32u));
        appendU64(packet, 0u);
    }

}

void register_ps2_memory_tests()
{
    MiniTest::Case("PS2Memory", [](TestCase &tc)
    {
        tc.Run("uncached aliases map to same RDRAM bytes", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            mem.write32(0x00001000u, 0xDEADBEEFu);
            t.Equals(mem.read32(0x00001000u), 0xDEADBEEFu, "base readback should match");
            t.Equals(mem.read32(0x20001000u), 0xDEADBEEFu, "0x2000_0000 alias should map to RDRAM");

            // 0x3010_0000 maps to physical 0x0010_0000 (AboutPS2 memory map).
            mem.write32(0x00101000u, 0xDEADBEEFu);
            t.Equals(mem.read32(0x30101000u), 0xDEADBEEFu, "0x3010_0000 accelerated alias should map to RDRAM");

            mem.write32(0x20002000u, 0x13579BDFu);
            t.Equals(mem.read32(0x00002000u), 0x13579BDFu, "writes through 0x2000 alias should land in base RDRAM");

            mem.write32(0x30103000u, 0x2468ACE0u);
            t.Equals(mem.read32(0x00103000u), 0x2468ACE0u, "writes through 0x3010 alias should land in base RDRAM");
        });

        tc.Run("EE INTC status acknowledges and mask toggles on write-one", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "INTC register fixture should initialize");

            constexpr uint32_t kIntcStat = 0x1000F000u;
            constexpr uint32_t kIntcMask = 0x1000F010u;
            constexpr uint32_t kVu1Cause = 1u << 7u;
            constexpr uint32_t kTimerCause = 1u << 9u;

            t.Equals(mem.read32(kIntcStat), 0u, "INTC_STAT starts clear");
            t.Equals(mem.read32(kIntcMask), 0u, "INTC_MASK starts clear");

            // A disabled interrupt still latches its source for guest polling.
            mem.latchEeIntcCause(7u);
            mem.latchEeIntcCause(9u);
            mem.latchEeIntcCause(16u); // Outside the sixteen hardware sources.
            t.Equals(mem.read32(kIntcStat), kVu1Cause | kTimerCause,
                     "INTC_STAT should retain independent pending causes");

            mem.write32(kIntcStat, 0u);
            t.Equals(mem.read32(kIntcStat), kVu1Cause | kTimerCause,
                     "writing zero must not acknowledge a pending cause");
            mem.write32(kIntcStat, kVu1Cause);
            t.Equals(mem.read32(kIntcStat), kTimerCause,
                     "writing one must acknowledge only the selected source");
            mem.write32(kIntcStat, 0xFFFFFFFFu);
            t.Equals(mem.read32(kIntcStat), 0u,
                     "writing one to all status bits must clear pending causes");

            mem.write32(kIntcMask, kVu1Cause | kTimerCause);
            t.Equals(mem.read32(kIntcMask), kVu1Cause | kTimerCause,
                     "INTC_MASK must enable bits selected by write-one");
            mem.write32(kIntcMask, kVu1Cause);
            t.Equals(mem.read32(kIntcMask), kTimerCause,
                     "a second write-one must disable the selected bit");
            mem.write32(kIntcMask, 0u);
            t.Equals(mem.read32(kIntcMask), kTimerCause,
                     "writing zero must leave the mask unchanged");
            mem.write32(kIntcMask, 0xFFFF0000u);
            t.Equals(mem.read32(kIntcMask), kTimerCause,
                     "high reserved bits must not modify the mask");
            mem.write32(kIntcMask, kTimerCause);
            t.Equals(mem.read32(kIntcMask), 0u,
                     "write-one toggling should restore the initial mask");
        });

        tc.Run("narrow interrupt writes preserve unrelated W1C and W1T bits", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "narrow interrupt register fixture should initialize");

            constexpr uint32_t kIntcStat = 0x1000F000u;
            constexpr uint32_t kIntcMask = 0x1000F010u;
            constexpr uint32_t kDStat = 0x1000E010u;
            constexpr uint32_t kVu1 = 1u << 7u;
            constexpr uint32_t kTimer0 = 1u << 9u;

            mem.latchEeIntcCause(7u);
            mem.latchEeIntcCause(9u);
            mem.write8(kIntcStat, 0u);
            t.Equals(mem.read32(kIntcStat), kVu1 | kTimer0,
                     "byte write-zero must not acknowledge other pending INTC sources");
            mem.write8(kIntcStat, 0x80u);
            t.Equals(mem.read32(kIntcStat), kTimer0,
                     "low-byte acknowledgement must preserve a high-byte source");
            mem.write16(kIntcStat, 0u);
            t.Equals(mem.read32(kIntcStat), kTimer0,
                     "halfword write-zero must not clear a pending INTC source");
            mem.write8(kIntcStat + 1u, 0x02u);
            t.Equals(mem.read32(kIntcStat), 0u,
                     "high-byte acknowledgement must clear only the addressed source");

            mem.write32(kIntcMask, kVu1 | kTimer0);
            mem.write8(kIntcMask, 0u);
            t.Equals(mem.read32(kIntcMask), kVu1 | kTimer0,
                     "byte write-zero must not toggle existing enabled sources");
            mem.write8(kIntcMask, 0x80u);
            t.Equals(mem.read32(kIntcMask), kTimer0,
                     "low-byte toggle must preserve a high-byte mask bit");
            mem.write16(kIntcMask, 0u);
            t.Equals(mem.read32(kIntcMask), kTimer0,
                     "halfword write-zero must preserve enabled sources");
            mem.write16(kIntcMask + 2u, 0xFFFFu);
            t.Equals(mem.read32(kIntcMask), kTimer0,
                     "write to reserved high halfword must not toggle active INTC mask");
            mem.write16(kIntcMask, static_cast<uint16_t>(kTimer0));
            t.Equals(mem.read32(kIntcMask), 0u,
                     "low-halfword toggle must disable only its addressed source");

            // D_STAT is also write-one-to-clear/toggle: an unrelated byte
            // store must not flip its retained high-half channel mask.
            mem.write32(kDStat, 1u << 16u);
            t.IsTrue((mem.read32(kDStat) & (1u << 16u)) != 0u,
                     "D_STAT channel mask must toggle on");
            mem.write8(kDStat, 0u);
            t.IsTrue((mem.read32(kDStat) & (1u << 16u)) != 0u,
                     "D_STAT byte write-zero must not toggle the unrelated channel mask");
            mem.write16(kDStat + 2u, 1u);
            t.Equals(mem.read32(kDStat) & (1u << 16u), 0u,
                     "D_STAT high halfword must toggle only the requested mask bit");
        });

        tc.Run("translateAddress handles kseg and uncached aliases", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            t.Equals(mem.translateAddress(0x80001234u), 0x00001234u, "KSEG0 should map directly to physical");
            t.Equals(mem.translateAddress(0xA0005678u), 0x00005678u, "KSEG1 should map directly to physical");
            t.Equals(mem.translateAddress(0x20001234u), 0x00001234u, "0x2000 uncached alias should map to RAM");
            t.Equals(mem.translateAddress(0x30105678u), 0x00105678u, "0x3010 accelerated alias should map to RAM");
            t.Equals(mem.translateAddress(PS2_SCRATCHPAD_BASE + 0x123u), 0x123u, "scratchpad base should translate to local offset");
            t.Equals(mem.translateAddress(PS2_SCRATCHPAD_ALIAS_BASE + 0x123u), 0x123u, "0xF000 scratchpad alias should translate to local offset");
        });

        tc.Run("EE timer0 count advances from scheduler cycles and can be reset", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kTimer0Count = 0x10000000u;
            constexpr uint32_t kTimer0Mode = 0x10000010u;
            constexpr uint32_t kTimer0Compare = 0x10000020u;

            t.IsTrue(mem.writeIORegister(kTimer0Count, 0u), "timer count reset write should succeed");
            t.IsTrue(mem.writeIORegister(kTimer0Compare, 0xFFFFu), "timer compare write should succeed");
            t.IsTrue(mem.writeIORegister(kTimer0Mode, 0x82u), "timer mode write should be retained");
            t.Equals(mem.readIORegister(kTimer0Mode), 0x82u, "timer mode should be readable");

            mem.advanceEeTimers(8u * 512u);
            const uint32_t firstCount = mem.readIORegister(kTimer0Count);
            t.Equals(firstCount, 8u, "BUSCLK/256 should increment once per 512 EE cycles");

            t.IsTrue(mem.writeIORegister(kTimer0Count, 0u), "timer count second reset should succeed");
            mem.advanceEeTimers(512u);
            const uint32_t resetCount = mem.readIORegister(kTimer0Count);
            t.Equals(resetCount, 1u, "timer reset should restart the deterministic count window");
        });

        tc.Run("EE timers 0 through 3 expose independent COUNT MODE and COMP registers", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kTimerBases[] = {
                0x10000000u,
                0x10000800u,
                0x10001000u,
                0x10001800u,
            };
            constexpr uint32_t kBusClockDiv256Cue = 0x82u;
            for (uint32_t index = 0u; index < 4u; ++index)
            {
                const uint32_t base = kTimerBases[index];
                t.IsTrue(mem.writeIORegister(base, 0x100u + index), "timer COUNT write should succeed");
                t.IsTrue(mem.writeIORegister(base + 0x10u, kBusClockDiv256Cue), "timer MODE write should succeed");
                t.IsTrue(mem.writeIORegister(base + 0x20u, 0x200u + index), "timer COMP write should succeed");
            }

            mem.advanceEeTimers(512u);
            for (uint32_t index = 0u; index < 4u; ++index)
            {
                const uint32_t base = kTimerBases[index];
                t.Equals(mem.readIORegister(base), 0x101u + index, "each timer should advance its own COUNT");
                t.Equals(mem.readIORegister(base + 0x10u), kBusClockDiv256Cue, "each timer should retain MODE");
                t.Equals(mem.readIORegister(base + 0x20u), 0x200u + index, "each timer should retain COMP");
            }

            t.IsTrue(mem.writeIORegister(kTimerBases[0] + 0x30u, 0x12345u), "Timer0 HOLD write should succeed");
            t.IsTrue(mem.writeIORegister(kTimerBases[1] + 0x30u, 0x23456u), "Timer1 HOLD write should succeed");
            t.Equals(mem.readIORegister(kTimerBases[0] + 0x30u), 0x2345u, "Timer0 HOLD should be 16-bit");
            t.Equals(mem.readIORegister(kTimerBases[1] + 0x30u), 0x3456u, "Timer1 HOLD should be 16-bit");
        });

        tc.Run("EE Timer2 compare and overflow flags raise INTC_TIM2 and clear on write-one", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kTimer2Count = 0x10001000u;
            constexpr uint32_t kTimer2Mode = 0x10001010u;
            constexpr uint32_t kTimer2Compare = 0x10001020u;
            constexpr uint32_t kCue = 1u << 7u;
            constexpr uint32_t kCmpe = 1u << 8u;
            constexpr uint32_t kOvfe = 1u << 9u;
            constexpr uint32_t kEquf = 1u << 10u;
            constexpr uint32_t kOvff = 1u << 11u;
            constexpr uint32_t kBusClockDiv256 = 2u;

            mem.writeIORegister(kTimer2Count, 0u);
            mem.writeIORegister(kTimer2Compare, 8u);
            mem.writeIORegister(kTimer2Mode, kBusClockDiv256 | kCue | kCmpe | kEquf | kOvff);

            t.Equals(mem.advanceEeTimers(7u * 512u), 0u, "compare should not fire before COUNT reaches COMP");
            t.Equals(mem.readIORegister(kTimer2Count), 7u, "Timer2 should expose its live 16-bit count");
            t.Equals(mem.advanceEeTimers(512u), 1u << 2u, "Timer2 compare should raise the TIM2 interrupt bit");
            t.IsTrue((mem.readIORegister(kTimer2Mode) & kEquf) != 0u, "Timer2 compare should latch EQUF");

            mem.writeIORegister(kTimer2Mode, mem.readIORegister(kTimer2Mode) | kEquf);
            t.IsTrue((mem.readIORegister(kTimer2Mode) & kEquf) == 0u, "writing one should clear EQUF");

            mem.writeIORegister(kTimer2Count, 0xFFFFu);
            mem.writeIORegister(kTimer2Mode, kBusClockDiv256 | kCue | kOvfe | kOvff);
            t.Equals(mem.advanceEeTimers(512u), 1u << 2u, "Timer2 overflow should raise the TIM2 interrupt bit");
            t.Equals(mem.readIORegister(kTimer2Count), 0u, "Timer2 count should wrap at 16 bits");
            t.IsTrue((mem.readIORegister(kTimer2Mode) & kOvff) != 0u, "Timer2 overflow should latch OVFF");

            mem.writeIORegister(kTimer2Mode, mem.readIORegister(kTimer2Mode) | kOvff);
            t.IsTrue((mem.readIORegister(kTimer2Mode) & kOvff) == 0u, "writing one should clear OVFF");
        });

        tc.Run("EE timer zero-return clears COUNT on compare", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kTimer0Count = 0x10000000u;
            constexpr uint32_t kTimer0Mode = 0x10000010u;
            constexpr uint32_t kTimer0Compare = 0x10000020u;
            constexpr uint32_t kZret = 1u << 6u;
            constexpr uint32_t kCue = 1u << 7u;
            constexpr uint32_t kCmpe = 1u << 8u;
            constexpr uint32_t kEquf = 1u << 10u;

            mem.writeIORegister(kTimer0Count, 0u);
            mem.writeIORegister(kTimer0Compare, 3u);
            mem.writeIORegister(kTimer0Mode, kZret | kCue | kCmpe | kEquf);

            t.Equals(mem.advanceEeTimers(6u), 1u, "Timer0 compare should raise TIM0 after three BUSCLK ticks");
            t.Equals(mem.readIORegister(kTimer0Count), 0u, "ZRET should clear COUNT when it equals COMP");
        });

        tc.Run("scratchpad alias accesses the same bytes as base", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kOffset = 0x140u;
            constexpr uint32_t kScratchAddr = PS2_SCRATCHPAD_BASE + kOffset;
            constexpr uint32_t kScratchAliasAddr = PS2_SCRATCHPAD_ALIAS_BASE + kOffset;

            mem.write32(kScratchAliasAddr, 0xCAFEBABEu);
            t.Equals(mem.read32(kScratchAddr), 0xCAFEBABEu, "writes through 0xF000 scratchpad alias should land in scratchpad");
            t.Equals(mem.read32(kScratchAliasAddr), 0xCAFEBABEu, "reads through 0xF000 scratchpad alias should see scratchpad bytes");
        });

        tc.Run("VU0 code and data windows map through EE addresses", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kCodeAddr = PS2_VU0_CODE_BASE + 0x20u;
            mem.write32(kCodeAddr, 0x11223344u);
            t.Equals(mem.read32(kCodeAddr), 0x11223344u, "VU0 code readback should match written word");

            uint32_t codeWord = 0u;
            std::memcpy(&codeWord, mem.getVU0Code() + 0x20u, sizeof(codeWord));
            t.Equals(codeWord, 0x11223344u, "VU0 code write should land in micro memory buffer");

            constexpr uint32_t kDataAddr = PS2_VU0_DATA_BASE + 0x30u;
            const __m128i value = _mm_set_epi32(0x44556677u, 0x01234567u, 0x89ABCDEFu, 0xCAFEBABEu);
            mem.write128(kDataAddr, value);

            alignas(16) uint32_t words[4]{};
            const __m128i readback = mem.read128(kDataAddr);
            _mm_storeu_si128(reinterpret_cast<__m128i *>(words), readback);

            t.Equals(words[0], 0xCAFEBABEu, "VU0 data lane 0 should match");
            t.Equals(words[1], 0x89ABCDEFu, "VU0 data lane 1 should match");
            t.Equals(words[2], 0x01234567u, "VU0 data lane 2 should match");
            t.Equals(words[3], 0x44556677u, "VU0 data lane 3 should match");
        });

        tc.Run("fast memory helpers wrap safely at RAM boundary", [](TestCase &t)
        {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0u);
            const uint32_t tail = PS2_RAM_SIZE - 4u;

            // Build a wrapped 64-bit pattern: [tail..tail+3] + [0..3]
            rdram[tail + 0u] = 0xA1u;
            rdram[tail + 1u] = 0xB2u;
            rdram[tail + 2u] = 0xC3u;
            rdram[tail + 3u] = 0xD4u;
            rdram[0u] = 0x11u;
            rdram[1u] = 0x22u;
            rdram[2u] = 0x33u;
            rdram[3u] = 0x44u;

            const uint64_t wrappedRead = Ps2FastRead64(rdram.data(), tail);
            t.Equals(wrappedRead, 0x44332211D4C3B2A1ull,
                     "Ps2FastRead64 should wrap across the 32MB boundary");

            Ps2FastWrite64(rdram.data(), tail, 0x8877665544332211ull);
            t.Equals(static_cast<uint32_t>(rdram[tail + 0u]), 0x11u, "write byte 0 should land at tail+0");
            t.Equals(static_cast<uint32_t>(rdram[tail + 1u]), 0x22u, "write byte 1 should land at tail+1");
            t.Equals(static_cast<uint32_t>(rdram[tail + 2u]), 0x33u, "write byte 2 should land at tail+2");
            t.Equals(static_cast<uint32_t>(rdram[tail + 3u]), 0x44u, "write byte 3 should land at tail+3");
            t.Equals(static_cast<uint32_t>(rdram[0u]), 0x55u, "write byte 4 should wrap to address 0");
            t.Equals(static_cast<uint32_t>(rdram[1u]), 0x66u, "write byte 5 should wrap to address 1");
            t.Equals(static_cast<uint32_t>(rdram[2u]), 0x77u, "write byte 6 should wrap to address 2");
            t.Equals(static_cast<uint32_t>(rdram[3u]), 0x88u, "write byte 7 should wrap to address 3");
        });

        tc.Run("VIF MPG num zero uploads 256 instructions", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            std::vector<uint8_t> packet;
            packet.reserve(4u + 2048u);
            appendU32(packet, makeVifCmd(0x4Au, 0u, 0u)); // MPG, num=0 -> 256 instructions (2048 bytes)

            for (uint32_t i = 0; i < 2048u; ++i)
            {
                packet.push_back(static_cast<uint8_t>(i & 0xFFu));
            }

            std::memset(mem.getVU1Code(), 0, PS2_VU1_CODE_SIZE);
            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            const uint8_t *vu1Code = mem.getVU1Code();
            bool matches = true;
            for (uint32_t i = 0; i < 2048u; ++i)
            {
                if (vu1Code[i] != static_cast<uint8_t>(i & 0xFFu))
                {
                    matches = false;
                    break;
                }
            }
            t.IsTrue(matches, "MPG num=0 should copy 2048 bytes into VU1 code memory");
        });

        tc.Run("VIF UNPACK num zero uploads 256 vectors", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            // UNPACK V4_32: opcode 0x6C (vn=3, vl=0), num=0 => 256 vectors, 16 bytes each.
            std::vector<uint8_t> packet;
            packet.reserve(4u + 4096u);
            appendU32(packet, makeVifCmd(0x6Cu, 0u, 0u));
            for (uint32_t i = 0; i < 4096u; ++i)
            {
                packet.push_back(static_cast<uint8_t>((i * 3u) & 0xFFu));
            }

            std::memset(mem.getVU1Data(), 0, PS2_VU1_DATA_SIZE);
            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            const uint8_t *vu1Data = mem.getVU1Data();
            bool matches = true;
            for (uint32_t i = 0; i < 4096u; ++i)
            {
                if (vu1Data[i] != static_cast<uint8_t>((i * 3u) & 0xFFu))
                {
                    matches = false;
                    break;
                }
            }
            t.IsTrue(matches, "UNPACK num=0 should copy 256 V4_32 vectors (4096 bytes)");
        });

        tc.Run("VIF control commands update MARK MASK ROW and COL registers", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            std::vector<uint8_t> packet;
            appendU32(packet, makeVifCmd(0x07u, 0u, 0x1234u)); // MARK

            appendU32(packet, makeVifCmd(0x20u, 0u, 0u));      // STMASK
            appendU32(packet, 0x89ABCDEFu);

            appendU32(packet, makeVifCmd(0x30u, 0u, 0u));      // STROW
            appendU32(packet, 0x11111111u);
            appendU32(packet, 0x22222222u);
            appendU32(packet, 0x33333333u);
            appendU32(packet, 0x44444444u);

            appendU32(packet, makeVifCmd(0x31u, 0u, 0u));      // STCOL
            appendU32(packet, 0xAAAA0001u);
            appendU32(packet, 0xAAAA0002u);
            appendU32(packet, 0xAAAA0003u);
            appendU32(packet, 0xAAAA0004u);

            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            t.Equals(mem.vif1_regs.mark, 0x1234u, "MARK should set VIF1 MARK register");
            t.Equals(mem.vif1_regs.mask, 0x89ABCDEFu, "STMASK should set VIF1 MASK register");

            t.Equals(mem.vif1_regs.row[0], 0x11111111u, "STROW should set row[0]");
            t.Equals(mem.vif1_regs.row[1], 0x22222222u, "STROW should set row[1]");
            t.Equals(mem.vif1_regs.row[2], 0x33333333u, "STROW should set row[2]");
            t.Equals(mem.vif1_regs.row[3], 0x44444444u, "STROW should set row[3]");

            t.Equals(mem.vif1_regs.col[0], 0xAAAA0001u, "STCOL should set col[0]");
            t.Equals(mem.vif1_regs.col[1], 0xAAAA0002u, "STCOL should set col[1]");
            t.Equals(mem.vif1_regs.col[2], 0xAAAA0003u, "STCOL should set col[2]");
            t.Equals(mem.vif1_regs.col[3], 0xAAAA0004u, "STCOL should set col[3]");
        });

        tc.Run("VIF1 fixed-width control payloads resume across DMA qwords", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "VIF1 fragmented-control fixture should initialize");

            // Each 16-byte block is a separate transport fragment. STMASK
            // and STROW begin at the end of a block, while STCOL has only
            // three of its four data words in the first block.
            constexpr uint32_t blocks[][4] = {
                {0x07001234u, 0x00000000u, 0x00000000u, 0x20000000u},
                {0x89ABCDEFu, 0x00000000u, 0x00000000u, 0x30000000u},
                {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u},
                {0x31000000u, 0xAAAA0001u, 0xAAAA0002u, 0xAAAA0003u},
                {0xAAAA0004u, 0x07005678u, 0x00000000u, 0x00000000u},
            };
            for (uint32_t block = 0u; block < 5u; ++block)
            {
                mem.processVIF1Data(reinterpret_cast<const uint8_t *>(blocks[block]),
                                    sizeof(blocks[block]));
                if (block == 0u)
                {
                    t.Equals(mem.vif1_regs.mark, 0x1234u, "first MARK should execute");
                    t.Equals(mem.vif1_regs.mask, 0u,
                             "incomplete STMASK must not publish a partial mask");
                }
                if (block == 1u)
                {
                    t.Equals(mem.vif1_regs.mask, 0x89ABCDEFu,
                             "STMASK must accept the payload from the next qword");
                    t.Equals(mem.vif1_regs.row[0], 0u,
                             "incomplete STROW must leave existing row data intact");
                }
                if (block == 2u)
                {
                    t.Equals(mem.vif1_regs.row[0], 0x11111111u,
                             "STROW must resume at the next qword");
                    t.Equals(mem.vif1_regs.row[3], 0x44444444u,
                             "STROW must publish the final word");
                }
                if (block == 3u)
                {
                    t.Equals(mem.vif1_regs.col[0], 0u,
                             "partial STCOL must not publish before its final word");
                }
            }

            t.Equals(mem.vif1_regs.mark, 0x5678u,
                     "commands after the completed STCOL payload must be parsed");
            t.Equals(mem.vif1_regs.col[0], 0xAAAA0001u,
                     "STCOL first word should survive the chunk boundary");
            t.Equals(mem.vif1_regs.col[1], 0xAAAA0002u,
                     "STCOL second word should survive the chunk boundary");
            t.Equals(mem.vif1_regs.col[2], 0xAAAA0003u,
                     "STCOL third word should survive the chunk boundary");
            t.Equals(mem.vif1_regs.col[3], 0xAAAA0004u,
                     "STCOL last word should be consumed only once");
        });

        tc.Run("VIF1 reset discards an incomplete fixed-width command", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "VIF1 reset fixture should initialize");
            constexpr uint32_t first[] = {
                0x07001234u, 0x00000000u, 0x00000000u, 0x30000000u
            };
            constexpr uint32_t afterReset[] = {
                0x07005678u, 0x00000000u, 0x00000000u, 0x00000000u
            };

            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(first), sizeof(first));
            t.Equals(mem.vif1_regs.mark, 0x1234u, "initial MARK should execute");
            t.IsTrue(mem.writeIORegister(0x10003C10u, 1u),
                     "FBRST.RST should accept a reset request");
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(afterReset), sizeof(afterReset));
            t.Equals(mem.vif1_regs.mark, 0x5678u,
                     "reset must not treat subsequent commands as STROW data");
            t.Equals(mem.vif1_regs.row[0], 0u, "reset must retain the initial zero value for an untouched ROW lane");
        });

        tc.Run("VIF1 MPG microprogram bytes resume across two DMA packets", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "split MPG DMA fixture should initialize");

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kFirstSource = 0x0002A000u;
            constexpr uint32_t kSecondSource = 0x0002A010u;
            constexpr uint32_t first[] = {
                0x4A030002u, // MPG: three 8-byte instructions at code offset 16.
                0x11223344u, 0x55667788u, 0x99AABBCCu
            };
            constexpr uint32_t second[] = {
                0xDDEEFF00u, 0x13572468u, 0x24681357u,
                0x07001234u // MARK after the final MPG payload word.
            };
            std::memcpy(mem.getRDRAM() + kFirstSource, first, sizeof(first));
            std::memcpy(mem.getRDRAM() + kSecondSource, second, sizeof(second));

            const auto transmit = [&](uint32_t source)
            {
                mem.writeIORegister(kVif1Ch + 0x10u, source);
                mem.writeIORegister(kVif1Ch + 0x20u, 1u);
                mem.writeIORegister(kVif1Ch, 0x100u);
            };
            transmit(kFirstSource);

            t.Equals(mem.read32(PS2_VU1_CODE_BASE + 16u), 0x11223344u,
                     "MPG should upload its first data word immediately");
            t.Equals(mem.read32(PS2_VU1_CODE_BASE + 24u), 0x99AABBCCu,
                     "MPG should accept a partial instruction at the first DMA boundary");
            t.Equals(mem.read32(PS2_VU1_CODE_BASE + 28u), 0u,
                     "the incomplete instruction must not invent missing bytes");
            t.Equals(mem.vif1_regs.mark, 0u,
                     "pending MPG data must not decode the next packet prematurely");

            transmit(kSecondSource);

            constexpr uint32_t expected[] = {
                0x11223344u, 0x55667788u, 0x99AABBCCu,
                0xDDEEFF00u, 0x13572468u, 0x24681357u
            };
            for (uint32_t word = 0u; word < 6u; ++word)
                t.Equals(mem.read32(PS2_VU1_CODE_BASE + 16u + word * 4u),
                         expected[word], "all MPG words must retain their original order");

            t.Equals(mem.vif1_regs.mark, 0x1234u,
                     "the next VIFcode must decode after completing MPG data");
            t.Equals(mem.readIORegister(kVif1Ch + 0x20u), 0u,
                     "the second DMA should complete without leaving QWC pending");
        });

        tc.Run("VIF1 reset cancels an incomplete MPG upload", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "MPG reset fixture should initialize");
            constexpr uint32_t partial[] = {
                0x4A020000u, // MPG: two instructions; only 12 of 16 bytes provided.
                0x11111111u, 0x22222222u, 0x33333333u
            };
            constexpr uint32_t following[] = {
                0x07005678u, 0u, 0u, 0u
            };
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(partial), sizeof(partial));
            t.IsTrue(mem.writeIORegister(0x10003C10u, 1u), "VIF1 FBRST reset should succeed");
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(following), sizeof(following));

            t.Equals(mem.vif1_regs.mark, 0x5678u,
                     "reset must permit a new command instead of consuming stale MPG data");
            t.Equals(mem.read32(PS2_VU1_CODE_BASE), 0x11111111u,
                     "reset should not undo an already transferred microprogram word");
        });

        tc.Run("VIF1 DMA pauses after I-bit MARK and resumes on FBRST.STC", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "DMA interrupt stall fixture should initialize");
            constexpr uint32_t kChannel = 0x10009000u;
            constexpr uint32_t kDStat = 0x1000E010u;
            constexpr uint32_t kFbrst = 0x10003C10u;
            constexpr uint32_t kSource = 0x0002C400u;
            const uint32_t words[][4] = {
                {makeVifCmd(0x07u, 0u, 0x1111u) | 0x80000000u,
                 makeVifCmd(0x07u, 0u, 0x2222u), 0u, 0u},
                {makeVifCmd(0x07u, 0u, 0x3333u), 0u, 0u, 0u}
            };
            std::memcpy(mem.getRDRAM() + kSource, words, sizeof(words));
            mem.writeIORegister(kChannel + 0x10u, kSource);
            mem.writeIORegister(kChannel + 0x20u, 2u);
            mem.writeIORegister(kChannel, 0x100u);

            constexpr uint32_t kInt = 1u << 11u;
            constexpr uint32_t kVis = 1u << 10u;
            t.Equals(mem.read32(0x10003C30u), 0x1111u,
                     "I-bit MARK must execute before the stall");
            t.Equals(mem.read32(0x10003C00u) & (kVis | kInt), kVis | kInt,
                     "DMA I-bit must publish both INT and VIS");
            t.IsTrue((mem.readIORegister(kChannel) & 0x100u) != 0u,
                     "CHCR.STR must remain set while DMA is suspended");
            t.IsTrue(mem.readIORegister(kChannel + 0x20u) != 0u,
                     "QWC must not be cleared before the remaining bytes run");
            t.Equals(mem.readIORegister(kDStat) & 2u, 0u,
                     "a suspended DMA must not signal D_STAT completion");
            t.IsTrue(mem.consumeCompletedDmacCauses().empty(),
                     "a suspended DMA must not enqueue a completion cause");

            mem.processPendingTransfers();
            t.Equals(mem.read32(0x10003C30u), 0x1111u,
                     "polling must not bypass the VIS stall");

            t.IsTrue(mem.writeIORegister(kFbrst, 8u),
                     "FBRST.STC must resume the paused DMA");
            t.Equals(mem.read32(0x10003C00u) & (kVis | kInt), 0u,
                     "STC must clear the completed VIF interrupt stall");
            t.Equals(mem.read32(0x10003C30u), 0x3333u,
                     "the unread MARK commands must execute in order");
            t.Equals(mem.readIORegister(kChannel + 0x20u), 0u,
                     "the complete transfer must clear QWC");
            t.Equals(mem.readIORegister(kChannel) & 0x100u, 0u,
                     "the complete transfer must clear CHCR.STR");
            t.IsTrue((mem.readIORegister(kDStat) & 2u) != 0u,
                     "D_STAT must latch only after the resumed transfer completes");
            const std::vector<uint32_t> causes = mem.consumeCompletedDmacCauses();
            t.Equals(causes.size(), static_cast<size_t>(1u),
                     "resumed VIF1 DMA must emit exactly one completion");
            if (!causes.empty())
                t.Equals(causes[0], 1u, "completion cause must identify VIF1");
            mem.processPendingTransfers();
            t.IsTrue(mem.consumeCompletedDmacCauses().empty(),
                     "a redundant drain must not duplicate completion");
        });

        tc.Run("VIF1 DMA stalls after I-bit UNPACK and preserves the trailing opcode", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "UNPACK DMA stall fixture should initialize");
            constexpr uint32_t kChannel = 0x10009000u;
            constexpr uint32_t kDStat = 0x1000E010u;
            constexpr uint32_t kSource = 0x0002C500u;
            const uint32_t words[][4] = {
                {makeVifCmd(0x6Cu, 1u, 0x20u) | 0x80000000u,
                 0x11111111u, 0x22222222u, 0x33333333u},
                {0x44444444u, makeVifCmd(0x07u, 0u, 0xBEEFu), 0u, 0u}
            };
            std::memcpy(mem.getRDRAM() + kSource, words, sizeof(words));
            mem.writeIORegister(kChannel + 0x10u, kSource);
            mem.writeIORegister(kChannel + 0x20u, 2u);
            mem.writeIORegister(kChannel, 0x100u);
            uint32_t lane = 0u;
            std::memcpy(&lane, mem.getVU1Data() + 0x20u * 16u + 12u, 4u);
            t.Equals(lane, 0x44444444u,
                     "the complete V4-32 vector must be written before the stall");
            t.Equals(mem.read32(0x10003C30u), 0u,
                     "the trailing MARK must remain unread until STC");
            t.IsTrue((mem.read32(0x10003C00u) & (1u << 10u)) != 0u,
                     "UNPACK I-bit must suspend DMA with VIS");
            t.Equals(mem.readIORegister(kDStat) & 2u, 0u,
                     "D_STAT must not complete while trailing opcodes are unread");
            mem.writeIORegister(0x10003C10u, 8u);
            t.Equals(mem.read32(0x10003C30u), 0xBEEFu,
                     "STC must resume at the first unread opcode");
            t.IsTrue((mem.readIORegister(kDStat) & 2u) != 0u,
                     "the resumed UNPACK DMA must complete after MARK");
        });

        tc.Run("VIF1 chained DMA keeps TTE payload after an interrupt stall", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "VIF1 TTE interrupt stall fixture should initialize");
            constexpr uint32_t kChannel = 0x10009000u;
            constexpr uint32_t kDStat = 0x1000E010u;
            constexpr uint32_t kTag = 0x0002C600u;
            uint8_t *rdram = mem.getRDRAM();
            writeDmaTag(rdram, kTag, makeDmaTag(1u, 7u, 0u, false));
            const uint32_t high[] = {
                0u, makeVifCmd(0x07u, 0u, 0xAAAAu) | 0x80000000u
            };
            const uint32_t payload[] = {
                makeVifCmd(0x07u, 0u, 0xBBBBu), 0u, 0u, 0u
            };
            std::memcpy(rdram + kTag + 8u, high, sizeof high);
            std::memcpy(rdram + kTag + 16u, payload, sizeof payload);
            mem.writeIORegister(kChannel + 0x30u, kTag);
            mem.writeIORegister(kChannel, 0x144u); // STR | CHAIN | TTE
            t.Equals(mem.read32(0x10003C30u), 0xAAAAu,
                     "the I-bit command in tag-high data must execute first");
            t.IsTrue((mem.read32(0x10003C00u) & (1u << 10u)) != 0u,
                     "interrupt in TTE data must suspend the chain payload");
            t.Equals(mem.readIORegister(kDStat) & 2u, 0u,
                     "the suspended chain must not report completion");
            mem.writeIORegister(0x10003C10u, 8u);
            t.Equals(mem.read32(0x10003C30u), 0xBBBBu,
                     "STC must preserve the chain payload after tag-high data");
            t.IsTrue((mem.readIORegister(kDStat) & 2u) != 0u,
                     "the resumed chain must report one DMA completion");
        });

        tc.Run("VIF1 DMA retains final-command stalls and multiple I-bits", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "multiple-interrupt DMA fixture should initialize");
            constexpr uint32_t kChannel = 0x10009000u;
            constexpr uint32_t kDStat = 0x1000E010u;
            constexpr uint32_t kSource = 0x0002C700u;
            const uint32_t commands[][4] = {
                {0u, 0u, 0u, makeVifCmd(0x07u, 0u, 0x1111u) | 0x80000000u},
                {makeVifCmd(0x07u, 0u, 0x2222u) | 0x80000000u,
                 makeVifCmd(0x07u, 0u, 0x3333u), 0u, 0u}
            };
            std::memcpy(mem.getRDRAM() + kSource, commands, sizeof commands);
            mem.writeIORegister(kChannel + 0x10u, kSource);
            mem.writeIORegister(kChannel + 0x20u, 2u);
            mem.writeIORegister(kChannel, 0x100u);

            t.Equals(mem.read32(0x10003C30u), 0x1111u,
                     "I-bit at a quadword boundary must stall before the next opcode");
            t.IsTrue((mem.read32(0x10003C00u) & (1u << 10u)) != 0u,
                     "VIS should stay set at a transport boundary");
            t.Equals(mem.readIORegister(kDStat) & 2u, 0u,
                     "first interrupt must not finish the DMA");

            mem.writeIORegister(0x10003C10u, 8u);
            t.Equals(mem.read32(0x10003C30u), 0x2222u,
                     "first STC must stop again at the second I-bit");
            t.IsTrue((mem.read32(0x10003C00u) & (1u << 10u)) != 0u,
                     "second I-bit should generate another VIS stall");
            t.Equals(mem.readIORegister(kDStat) & 2u, 0u,
                     "second interrupt must not finish the DMA");
            t.IsTrue(mem.consumeCompletedDmacCauses().empty(),
                     "neither intermediate stall may emit DMAC completion");

            mem.writeIORegister(0x10003C10u, 8u);
            t.Equals(mem.read32(0x10003C30u), 0x3333u,
                     "second STC must finish the unread packet");
            t.Equals(mem.read32(0x10003C00u) & ((1u << 10u) | (1u << 11u)), 0u,
                     "a completed resume must leave VIS and INT clear");
            t.IsTrue((mem.readIORegister(kDStat) & 2u) != 0u,
                     "D_STAT should complete only after all interrupts are acknowledged");
            t.Equals(mem.consumeCompletedDmacCauses().size(), static_cast<size_t>(1u),
                     "a multiply-interrupted DMA still completes exactly once");
        });

        tc.Run("VIF1_ERR.MII prevents a DMA interrupt stall", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "masked DMA interrupt fixture should initialize");
            constexpr uint32_t kChannel = 0x10009000u;
            constexpr uint32_t kDStat = 0x1000E010u;
            constexpr uint32_t kSource = 0x0002C800u;
            const uint32_t commands[] = {
                makeVifCmd(0x07u, 0u, 0x1111u) | 0x80000000u,
                makeVifCmd(0x07u, 0u, 0x2222u), 0u, 0u
            };
            std::memcpy(mem.getRDRAM() + kSource, commands, sizeof commands);
            mem.writeIORegister(0x10003C20u, 1u); // MII
            mem.writeIORegister(kChannel + 0x10u, kSource);
            mem.writeIORegister(kChannel + 0x20u, 1u);
            mem.writeIORegister(kChannel, 0x100u);

            t.Equals(mem.read32(0x10003C30u), 0x2222u,
                     "masked command must not stall subsequent VIFcodes");
            t.Equals(mem.read32(0x10003C00u) & ((1u << 10u) | (1u << 11u)), 0u,
                     "MII must suppress both interrupt and resulting VIS stall");
            t.Equals(mem.readIORegister(kChannel + 0x20u), 0u,
                     "masked I-bit must not prevent a normal DMA completion");
            t.IsTrue((mem.readIORegister(kDStat) & 2u) != 0u,
                     "masked I-bit DMA must finish without STC");
        });

        tc.Run("VIF1 I-bit UNPACK can finish in a later DMA and still stall the tail", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "split DMA interrupt fixture should initialize");
            constexpr uint32_t kChannel = 0x10009000u;
            constexpr uint32_t kDStat = 0x1000E010u;
            constexpr uint32_t kSource = 0x0002C900u;
            const uint32_t blocks[][4] = {
                {0u, 0u, 0u, makeVifCmd(0x6Cu, 1u, 0x20u) | 0x80000000u},
                {1u, 2u, 3u, 4u},
                {makeVifCmd(0x07u, 0u, 0xBEEFu), 0u, 0u, 0u}
            };
            std::memcpy(mem.getRDRAM() + kSource, blocks, sizeof blocks);
            for (uint32_t i = 0u; i < 2u; ++i)
            {
                mem.writeIORegister(kChannel + 0x10u, kSource + i * 16u);
                mem.writeIORegister(kChannel + 0x20u, 1u);
                mem.writeIORegister(kChannel, 0x100u);
                if (i == 0u)
                {
                    t.Equals(mem.read32(0x10003C00u) & (1u << 10u), 0u,
                             "incomplete UNPACK must not assert VIS");
                    t.IsTrue((mem.readIORegister(kDStat) & 2u) != 0u,
                             "the transport DMA with only the header may finish");
                    mem.write32(kDStat, 2u);
                    t.Equals(mem.consumeCompletedDmacCauses().size(),
                             static_cast<size_t>(1u),
                             "the first DMA must notify its own completion");
                }
            }

            t.IsTrue((mem.read32(0x10003C00u) & (1u << 10u)) != 0u,
                     "the second DMA must pause after completing I-bit UNPACK");
            t.Equals(mem.readIORegister(kDStat) & 2u, 0u,
                     "the paused second DMA must not signal completion");
            uint32_t lastLane = 0u;
            std::memcpy(&lastLane, mem.getVU1Data() + 0x20u * 16u + 12u, 4u);
            t.Equals(lastLane, 4u, "the completed UNPACK vector must be intact");

            // The third DMA starts only after acknowledging the second.
            mem.writeIORegister(0x10003C10u, 8u);
            t.IsTrue((mem.readIORegister(kDStat) & 2u) != 0u,
                     "STC must complete the suspended second DMA");
            mem.write32(kDStat, 2u);
            mem.writeIORegister(kChannel + 0x10u, kSource + 32u);
            mem.writeIORegister(kChannel + 0x20u, 1u);
            mem.writeIORegister(kChannel, 0x100u);
            t.Equals(mem.read32(0x10003C30u), 0xBEEFu,
                     "the following DMA must decode its independent MARK");
            t.IsTrue((mem.readIORegister(kDStat) & 2u) != 0u,
                     "the final DMA must complete normally");
        });

        tc.Run("VIF1 normal-mode DMA resumes split UNPACK and later commands", [](TestCase &t)
        {
            PS2Memory mem;
            PS2Memory continuous;
            t.IsTrue(mem.initialize(), "VIF1 DMA stream fixture should initialize");
            t.IsTrue(continuous.initialize(), "contiguous UNPACK baseline should initialize");

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kSource = 0x0002C000u;
            constexpr uint32_t kDStat = 0x1000E010u;
            const uint32_t blocks[][4] = {
                {makeVifCmd(0x07u, 0u, 0x1111u), 0u, 0u,
                 makeVifCmd(0x6Cu, 2u, 0x0020u)}, // V4-32, two vectors.
                {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u},
                {0x55555555u, 0x66666666u, 0x77777777u, 0x88888888u},
                {makeVifCmd(0x07u, 0u, 0x2222u), 0u, 0u, 0u}
            };
            std::memcpy(mem.getRDRAM() + kSource, blocks, sizeof(blocks));
            continuous.processVIF1Data(reinterpret_cast<const uint8_t *>(blocks),
                                       static_cast<uint32_t>(sizeof(blocks)));

            for (uint32_t block = 0u; block < 4u; ++block)
            {
                t.IsTrue(mem.writeIORegister(kVif1Ch + 0x10u,
                                            kSource + block * 16u),
                         "VIF1 MADR must accept the next DMA packet");
                t.IsTrue(mem.writeIORegister(kVif1Ch + 0x20u, 1u),
                         "VIF1 QWC must accept one transport quadword");
                t.IsTrue(mem.writeIORegister(kVif1Ch, 0x100u),
                         "VIF1 CHCR must start a normal DMA packet");

                t.Equals(mem.readIORegister(kVif1Ch + 0x20u), 0u,
                         "the synchronous VIF1 DMA must finish each packet");
                t.IsTrue((mem.readIORegister(kDStat) & (1u << 1u)) != 0u,
                         "VIF1 channel completion must be latched after each DMA");
                mem.write32(kDStat, 1u << 1u);

                if (block < 2u)
                {
                    uint32_t untouched = 0xFFFFFFFFu;
                    std::memcpy(&untouched,
                                mem.getVU1Data() + 0x20u * 16u, sizeof(untouched));
                    t.Equals(untouched, 0u,
                             "an incomplete UNPACK must not publish a partial vector");
                }
                if (block == 0u)
                    t.Equals(mem.vif1_regs.mark, 0x1111u,
                             "the preceding MARK must execute before the UNPACK");
                if (block == 2u)
                {
                    uint32_t lastLane = 0u;
                    std::memcpy(&lastLane,
                                mem.getVU1Data() + 0x21u * 16u + 12u,
                                sizeof(lastLane));
                    t.Equals(lastLane, 0x88888888u,
                             "the final UNPACK vector must complete on the third DMA");
                    t.Equals(mem.vif1_regs.mark, 0x1111u,
                             "the trailing MARK must not execute before its own DMA");
                }
            }

            t.IsTrue(std::memcmp(mem.getVU1Data(), continuous.getVU1Data(),
                                 PS2_VU1_DATA_SIZE) == 0,
                     "normal DMA fragments must reproduce contiguous UNPACK memory");
            t.Equals(mem.vif1_regs.mark, 0x2222u,
                     "the VIF interpreter must resume decoding after the UNPACK");
            mem.processPendingTransfers();
            t.IsTrue(mem.consumeCompletedDmacCauses().size() == 4u,
                     "each of the four VIF1 DMA packets should notify completion once");
            t.IsTrue(mem.consumeCompletedDmacCauses().empty(),
                     "a second completion poll must not replay VIF1 DMA causes");
        });

        tc.Run("I-bit data-bearing VIF1 commands wait for their complete payload", [](TestCase &t)
        {
            constexpr uint32_t kInt = 1u << 11u;
            struct Case { uint8_t opcode; uint8_t num; uint16_t imm; uint32_t payloadWords; };
            constexpr Case cases[] = {
                {0x20u, 0u, 0u, 1u}, // STMASK
                {0x30u, 0u, 0u, 4u}, // STROW
                {0x31u, 0u, 0u, 4u}, // STCOL
                {0x4Au, 2u, 0u, 4u}, // MPG
                {0x50u, 0u, 1u, 4u}, // DIRECT
                {0x51u, 0u, 1u, 4u}, // DIRECTHL
            };
            for (const Case &tcData : cases)
            {
                PS2Memory mem;
                t.IsTrue(mem.initialize(), "fragmented I-bit VIF1 fixture should initialize");
                const uint32_t header[] = {
                    0u, 0u, 0u,
                    makeVifCmd(tcData.opcode, tcData.num, tcData.imm) | 0x80000000u
                };
                mem.processVIF1Data(reinterpret_cast<const uint8_t *>(header),
                                    sizeof(header));
                t.Equals(mem.vif1_regs.stat & kInt, 0u,
                         "the I-bit must not latch when the command has no payload yet");

                std::vector<uint32_t> payload(tcData.payloadWords, 0u);
                for (uint32_t i = 0u; i < tcData.payloadWords; ++i)
                    payload[i] = (tcData.opcode == 0x50u || tcData.opcode == 0x51u)
                        ? 0u : (i + 1u);
                for (uint32_t i = 0u; i < tcData.payloadWords; ++i)
                {
                    mem.processVIF1Data(
                        reinterpret_cast<const uint8_t *>(&payload[i]), 4u);
                    t.Equals(mem.vif1_regs.stat & kInt,
                             (i + 1u == tcData.payloadWords) ? kInt : 0u,
                             "INT must latch only when the final payload word arrives");
                }

                t.IsTrue(mem.writeIORegister(0x10003C10u, 0x8u),
                         "FBRST.STC should acknowledge the completed I-bit");
                t.Equals(mem.vif1_regs.stat & kInt, 0u,
                         "the completed I-bit must clear on FBRST.STC");
            }
        });

        tc.Run("VIF1 reset cancels pending I-bit fixed and streaming commands", [](TestCase &t)
        {
            constexpr uint32_t kInt = 1u << 11u;
            constexpr uint8_t opcodes[] = {0x20u, 0x30u, 0x31u, 0x4Au, 0x50u, 0x51u};
            for (const uint8_t opcode : opcodes)
            {
                PS2Memory mem;
                t.IsTrue(mem.initialize(), "reset pending VIF1 I-bit fixture should initialize");
                const uint8_t num = (opcode == 0x4Au) ? 2u : 0u;
                const uint16_t imm = (opcode == 0x50u || opcode == 0x51u) ? 1u : 0u;
                const uint32_t packet[] = {
                    0u, 0u, 0u, makeVifCmd(opcode, num, imm) | 0x80000000u
                };
                mem.processVIF1Data(reinterpret_cast<const uint8_t *>(packet),
                                    sizeof(packet));
                t.Equals(mem.vif1_regs.stat & kInt, 0u,
                         "a pending command must not set INT before reset");
                t.IsTrue(mem.writeIORegister(0x10003C10u, 1u),
                         "FBRST.RST should cancel the pending payload");
                const uint32_t marker = makeVifCmd(0x07u, 0u, 0xBEEF);
                mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&marker),
                                    sizeof(marker));
                t.Equals(mem.vif1_regs.mark, 0xBEEFu,
                         "post-reset MARK must decode outside the cancelled payload");
                t.Equals(mem.vif1_regs.stat & kInt, 0u,
                         "reset must discard the pending I-bit");
            }
        });

        tc.Run("VIF1_ERR.MII suppresses immediate I-bits without skipping commands", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "MII fixture should initialize");
            constexpr uint32_t kErr = 0x10003C20u;
            constexpr uint32_t kStat = 0x10003C00u;
            constexpr uint32_t kInt = 1u << 11u;
            const uint32_t cmd = makeVifCmd(0x07u, 0u, 0x1357u) | 0x80000000u;
            t.IsTrue(mem.writeIORegister(kErr, 1u), "MII should be writable");
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&cmd), sizeof(cmd));
            t.Equals(mem.read32(0x10003C30u), 0x1357u, "MII must not suppress MARK");
            t.Equals(mem.read32(kStat) & kInt, 0u, "masked I-bit must not set INT");
            t.IsTrue(mem.writeIORegister(kErr, 0u), "MII should be disableable");
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&cmd), sizeof(cmd));
            t.Equals(mem.read32(kStat) & kInt, kInt, "unmasked command must set INT");
            t.IsTrue(mem.writeIORegister(0x10003C10u, 8u), "STC should acknowledge INT");
            t.Equals(mem.read32(kStat) & kInt, 0u, "STC must clear INT");
        });

        tc.Run("VIF1_ERR.MII is sampled on each fragmented command header", [](TestCase &t)
        {
            constexpr uint32_t kErr = 0x10003C20u;
            constexpr uint32_t kStat = 0x10003C00u;
            constexpr uint32_t kInt = 1u << 11u;
            struct Command { uint8_t opcode; uint8_t count; uint16_t imm; uint32_t words; };
            constexpr Command commands[] = {
                {0x20u, 0u, 0u, 1u}, {0x30u, 0u, 0u, 4u},
                {0x31u, 0u, 0u, 4u}, {0x4Au, 2u, 0u, 4u},
                {0x50u, 0u, 1u, 4u}, {0x51u, 0u, 1u, 4u},
                {0x6Cu, 1u, 0x20u, 4u},
            };
            for (const Command &sample : commands)
            {
                for (const bool maskedAtHeader : {false, true})
                {
                    PS2Memory mem;
                    t.IsTrue(mem.initialize(), "fragmented MII fixture should initialize");
                    const uint32_t initial = maskedAtHeader ? 1u : 0u;
                    const uint32_t changed = maskedAtHeader ? 0u : 1u;
                    mem.writeIORegister(kErr, initial);
                    const uint32_t header[] = {
                        0u, 0u, 0u,
                        makeVifCmd(sample.opcode, sample.count, sample.imm) | 0x80000000u
                    };
                    mem.processVIF1Data(reinterpret_cast<const uint8_t *>(header), sizeof(header));
                    t.Equals(mem.read32(0x10003C80u), header[3],
                             "pending VIF1_CODE must retain the original I-bit header");
                    t.Equals(mem.read32(kStat) & kInt, 0u,
                             "incomplete payload must not set INT");
                    mem.writeIORegister(kErr, changed);
                    t.Equals(mem.read32(kErr), changed, "MII readback must change immediately");
                    std::vector<uint32_t> payload(sample.words, 0u);
                    if (sample.opcode != 0x50u && sample.opcode != 0x51u)
                        for (uint32_t i = 0; i < sample.words; ++i)
                            payload[i] = i + 1u;
                    mem.processVIF1Data(reinterpret_cast<const uint8_t *>(payload.data()),
                                        static_cast<uint32_t>(payload.size() * sizeof(uint32_t)));
                    t.Equals(mem.read32(kStat) & kInt, maskedAtHeader ? 0u : kInt,
                             "MII after the command header must not retroactively change INT");
                    mem.writeIORegister(0x10003C10u, 8u);
                    t.Equals(mem.read32(kStat) & kInt, 0u, "STC must clear INT");
                    t.Equals(mem.read32(kErr), changed, "STC must preserve MII");
                }
            }
        });

        tc.Run("VIF1 I-bit UNPACK asserts INT only after its payload arrives", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "VIF1 I-bit UNPACK fixture should initialize");

            constexpr uint32_t kInt = 1u << 11u;
            const uint32_t command = makeVifCmd(0x6Cu, 2u, 0x0020u) | 0x80000000u;
            const uint32_t first[] = {0u, 0u, 0u, command};
            constexpr uint32_t payloadA[] = {1u, 2u, 3u, 4u};
            constexpr uint32_t payloadB[] = {5u, 6u, 7u, 8u};
            const uint32_t following[] = {makeVifCmd(0x07u, 0u, 0xABCDu), 0u, 0u, 0u};

            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(first), sizeof first);
            t.Equals(mem.vif1_regs.code, command,
                     "VIF1_CODE must expose the current command header");
            t.Equals(mem.vif1_regs.stat & kInt, 0u,
                     "reading the UNPACK header must not assert INT early");

            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(payloadA),
                                sizeof payloadA);
            t.Equals(mem.vif1_regs.stat & kInt, 0u,
                     "INT must stay clear while UNPACK is waiting for its final vector");
            uint32_t untouched = 0xFFFFFFFFu;
            std::memcpy(&untouched, mem.getVU1Data() + 0x20u * 16u, sizeof untouched);
            t.Equals(untouched, 0u, "partial UNPACK must not write VU1 data memory");

            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(payloadB),
                                sizeof payloadB);
            t.Equals(mem.vif1_regs.stat & kInt, kInt,
                     "INT should latch when the final UNPACK payload byte is consumed");
            uint32_t result = 0u;
            std::memcpy(&result, mem.getVU1Data() + 0x21u * 16u + 12u, sizeof result);
            t.Equals(result, 8u, "both vectors should complete before INT is published");

            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(following),
                                sizeof following);
            t.Equals(mem.vif1_regs.mark, 0xABCDu, "subsequent commands should still decode");
            t.Equals(mem.vif1_regs.stat & kInt, kInt,
                     "INT must remain latched until guest acknowledgement");

            t.IsTrue(mem.writeIORegister(0x10003C10u, 0x8u),
                     "FBRST.STC should accept a stall-clear request");
            t.Equals(mem.vif1_regs.stat & kInt, 0u,
                     "stall-clear must acknowledge the pending VIF interrupt");
        });

        tc.Run("VIF1 reset cancels a fragmented I-bit UNPACK", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "cancelled UNPACK IRQ fixture should initialize");
            constexpr uint32_t kInt = 1u << 11u;
            const uint32_t first[] = {
                makeVifCmd(0x6Cu, 2u, 0x0000u) | 0x80000000u, 1u, 2u, 3u
            };
            const uint32_t afterReset[] = {
                makeVifCmd(0x07u, 0u, 0x2468u), 0u, 0u, 0u
            };
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(first), sizeof first);
            t.Equals(mem.vif1_regs.stat & kInt, 0u,
                     "incomplete I-bit UNPACK must not assert INT");
            t.IsTrue(mem.writeIORegister(0x10003C10u, 0x1u),
                     "VIF1 reset should cancel the incomplete command");
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(afterReset),
                                sizeof afterReset);
            t.Equals(mem.vif1_regs.mark, 0x2468u,
                     "post-reset MARK must not be consumed as old UNPACK data");
            t.Equals(mem.vif1_regs.stat & kInt, 0u,
                     "reset must discard the aborted UNPACK interrupt");
        });

        tc.Run("VIF1 FBRST reset preserves ROW/COL despite partial replacement", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "VIF1 reset-retention fixture should initialize");

            const uint32_t setup[] = {
                makeVifCmd(0x30u, 0u, 0u), 0x11111111u, 0x22222222u,
                0x33333333u, 0x44444444u,
                makeVifCmd(0x31u, 0u, 0u), 0xAAAA0001u, 0xAAAA0002u,
                0xAAAA0003u, 0xAAAA0004u
            };
            constexpr uint32_t rowExpected[] = {
                0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u
            };
            constexpr uint32_t colExpected[] = {
                0xAAAA0001u, 0xAAAA0002u, 0xAAAA0003u, 0xAAAA0004u
            };
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(setup),
                                sizeof(setup));
            t.IsTrue(mem.writeIORegister(0x10003C20u, 0x5u),
                     "a nonzero VIF1_ERR must be writable before reset");

            for (const uint8_t opcode : {uint8_t{0x30u}, uint8_t{0x31u}})
            {
                const uint32_t incompleteHeader[] = {
                    0u, 0u, 0u, makeVifCmd(opcode, 0u, 0u)
                };
                constexpr uint32_t incompleteData[] = {0xABCD1234u, 0xDCBA4321u};
                mem.processVIF1Data(
                    reinterpret_cast<const uint8_t *>(incompleteHeader),
                    sizeof(incompleteHeader));
                mem.processVIF1Data(
                    reinterpret_cast<const uint8_t *>(incompleteData),
                    sizeof(incompleteData));
                for (uint32_t i = 0u; i < 4u; ++i)
                {
                    t.Equals(mem.vif1_regs.row[i], rowExpected[i],
                             "partial STROW must not replace committed ROW lanes");
                    t.Equals(mem.vif1_regs.col[i], colExpected[i],
                             "partial STCOL must not replace committed COL lanes");
                }

                t.IsTrue(mem.writeIORegister(0x10003C10u, 1u),
                         "FBRST.RST must cancel the unfinished command");
                for (uint32_t i = 0u; i < 4u; ++i)
                {
                    t.Equals(mem.read32(0x10003D00u + i * 0x10u), rowExpected[i],
                             "ROW must survive FBRST.RST at its MMIO lane");
                    t.Equals(mem.read32(0x10003D40u + i * 0x10u), colExpected[i],
                             "COL must survive FBRST.RST at its MMIO lane");
                }
                t.Equals(mem.read32(0x10003C20u), 0u,
                         "FBRST.RST must still clear VIF1_ERR");
                t.Equals(mem.read32(0x10003C00u), 0u,
                         "FBRST.RST must still clear VIF1_STAT");

                const uint32_t mark = makeVifCmd(0x07u, 0u, 0x5678u);
                mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&mark),
                                    sizeof(mark));
                t.Equals(mem.read32(0x10003C30u), 0x5678u,
                         "the parser must resume at a new VIFcode after reset");
            }
            const uint32_t newRow[] = {
                makeVifCmd(0x30u, 0u, 0u), 9u, 8u, 7u, 6u
            };
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(newRow),
                                sizeof(newRow));
            t.Equals(mem.vif1_regs.row[0], 9u,
                     "a later complete STROW may replace the retained row");
            t.Equals(mem.vif1_regs.col[0], colExpected[0],
                     "updating ROW after reset must not overwrite COL");
        });

        tc.Run("VIF1 MMIO reads reflect parser state and register changes", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "VIF1 MMIO fixture should initialize");

            constexpr uint32_t kStat = 0x10003C00u;
            constexpr uint32_t kErr = 0x10003C20u;
            constexpr uint32_t kMark = 0x10003C30u;
            constexpr uint32_t kCode = 0x10003C80u;
            constexpr uint32_t kBase = 0x10003CA0u;
            constexpr uint32_t kOfst = 0x10003CB0u;
            constexpr uint32_t kTops = 0x10003CC0u;
            constexpr uint32_t kRow = 0x10003D00u;
            constexpr uint32_t kCol = 0x10003D40u;
            constexpr uint32_t kInt = 1u << 11u;

            t.Equals(mem.read32(kStat), 0u, "VIF1_STAT should initially be clear");
            t.IsTrue(mem.writeIORegister(kErr, 0xFFFFFFFFu),
                     "VIF1_ERR must accept CPU writes");
            t.Equals(mem.read32(kErr), 7u,
                     "VIF1_ERR exposes only the supported MII/ME0/ME1 bits");
            t.Equals(mem.vif1_regs.err, 7u,
                     "CPU writes must change the live VIF1_ERR register");
            mem.write32(kErr, 0u);

            const uint32_t header[] = {
                makeVifCmd(0x03u, 0u, 5u), // BASE
                makeVifCmd(0x02u, 0u, 3u), // OFFSET
                makeVifCmd(0x07u, 0u, 0x1234u), // MARK
                makeVifCmd(0x30u, 0u, 0u) | 0x80000000u // I-bit STROW
            };
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(header),
                                sizeof(header));
            t.Equals(mem.read32(kBase), 5u, "BASE must be guest-visible");
            t.Equals(mem.read32(kOfst), 3u, "OFFSET must be guest-visible");
            t.Equals(mem.read32(kTops), 5u, "TOPS must reflect OFFSET initialization");
            t.Equals(mem.read32(kMark), 0x1234u, "MARK should reflect VIFcode updates");
            t.Equals(mem.read32(kCode), header[3], "CODE must expose the pending VIFcode");
            t.Equals(mem.read32(kStat) & kInt, 0u,
                     "the guest must not see INT before a fragmented STROW completes");

            constexpr uint32_t rowPayload[] = {
                0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u
            };
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(rowPayload),
                                sizeof(rowPayload));
            t.Equals(mem.read32(kStat) & kInt, kInt,
                     "the guest must observe INT once STROW finishes");
            for (uint32_t i = 0u; i < 4u; ++i)
                t.Equals(mem.read32(kRow + 0x10u * i), rowPayload[i],
                         "STROW lanes must be visible through their MMIO registers");

            // A contiguous STCOL payload includes all four data words.
            const uint32_t colPacket[] = {
                makeVifCmd(0x31u, 0u, 0u), 9u, 8u, 7u, 6u
            };
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(colPacket),
                                sizeof(colPacket));
            for (uint32_t i = 0u; i < 4u; ++i)
                t.Equals(mem.read32(kCol + 0x10u * i), colPacket[i + 1u],
                         "STCOL lanes must be visible through their MMIO registers");

            t.IsTrue(mem.writeIORegister(0x10003C10u, 0x8u),
                     "FBRST.STC should acknowledge VIF1 interrupt status");
            t.Equals(mem.read32(kStat) & kInt, 0u,
                     "VIF1_STAT MMIO reads must reflect STC acknowledgement");
            t.IsTrue(mem.writeIORegister(0x10003C10u, 0x1u),
                     "FBRST.RST should reset VIF1 state");
            t.Equals(mem.read32(kMark), 0u, "reset must clear MARK readback");
            t.Equals(mem.read32(kCode), 0u, "reset must clear CODE readback");
            for (uint32_t i = 0u; i < 4u; ++i)
            {
                t.Equals(mem.read32(kRow + 0x10u * i), rowPayload[i],
                         "FBRST.RST must preserve each STROW lane");
                t.Equals(mem.read32(kCol + 0x10u * i), colPacket[i + 1u],
                         "FBRST.RST must preserve each STCOL lane");
            }
            t.Equals(mem.read32(kErr), 0u, "reset must clear VIF1_ERR readback");
        });

        tc.Run("VIF1 UNPACK V4-32 resumes through three DMA qwords", [](TestCase &t)
        {
            PS2Memory fragmented;
            PS2Memory contiguous;
            t.IsTrue(fragmented.initialize(), "fragmented VIF1 fixture should initialize");
            t.IsTrue(contiguous.initialize(), "contiguous VIF1 fixture should initialize");

            const uint32_t blocks[][4] = {
                {makeVifCmd(0x07u, 0u, 0x1111u), 0u, 0u,
                 makeVifCmd(0x6Cu, 2u, 0x0020u)},
                {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u},
                {0x55555555u, 0x66666666u, 0x77777777u, 0x88888888u},
                {makeVifCmd(0x07u, 0u, 0x2222u), 0u, 0u, 0u}
            };
            for (uint32_t block = 0u; block < 4u; ++block)
            {
                fragmented.processVIF1Data(
                    reinterpret_cast<const uint8_t *>(blocks[block]), 16u);
                if (block < 2u)
                {
                    uint32_t untouched = 0xFFFFFFFFu;
                    std::memcpy(&untouched, fragmented.getVU1Data() + 0x20u * 16u, 4u);
                    t.Equals(untouched, 0u,
                             "incomplete UNPACK must not publish partial VU1 vector data");
                }
            }
            contiguous.processVIF1Data(reinterpret_cast<const uint8_t *>(blocks),
                                       sizeof(blocks));
            t.IsTrue(std::memcmp(fragmented.getVU1Data(), contiguous.getVU1Data(),
                                 PS2_VU1_DATA_SIZE) == 0,
                     "fragmented V4-32 must match the contiguous destination memory");
            t.Equals(fragmented.vif1_regs.mark, 0x2222u,
                     "trailing MARK must execute after the completed UNPACK");
            uint32_t first = 0u, last = 0u;
            std::memcpy(&first, fragmented.getVU1Data() + 0x20u * 16u, 4u);
            std::memcpy(&last, fragmented.getVU1Data() + 0x21u * 16u + 12u, 4u);
            t.Equals(first, 0x11111111u, "first vector must reach the chosen VU1 address");
            t.Equals(last, 0x88888888u, "second vector must retain its final lane");
        });

        tc.Run("VIF1 UNPACK preserves zero extension and fill cycles across fragments", [](TestCase &t)
        {
            for (const bool zeroExtend : {false, true})
            {
                PS2Memory mem;
                t.IsTrue(mem.initialize(), "V4-16 fragment fixture should initialize");
                const uint16_t imm = zeroExtend ? 0x4004u : 0x0004u;
                const uint32_t first[] = {
                    0u, 0u, makeVifCmd(0x6Du, 1u, imm), 0xFF800001u
                };
                const uint32_t next[] = {
                    0x80017FFFu, makeVifCmd(0x07u, 0u, 0x1357u), 0u, 0u
                };
                mem.processVIF1Data(reinterpret_cast<const uint8_t *>(first), 16u);
                uint32_t incomplete = 0xFFFFFFFFu;
                std::memcpy(&incomplete, mem.getVU1Data() + 4u * 16u, 4u);
                t.Equals(incomplete, 0u, "half-complete V4-16 must not update VU1 data");

                mem.processVIF1Data(reinterpret_cast<const uint8_t *>(next), 16u);
                uint32_t y = 0u, w = 0u;
                std::memcpy(&y, mem.getVU1Data() + 4u * 16u + 4u, 4u);
                std::memcpy(&w, mem.getVU1Data() + 4u * 16u + 12u, 4u);
                t.Equals(y, zeroExtend ? 0x0000FF80u : 0xFFFFFF80u,
                         "V4-16 extension mode must survive fragmented transport");
                t.Equals(w, zeroExtend ? 0x00008001u : 0xFFFF8001u,
                         "final V4-16 lane must survive fragmented transport");
                t.Equals(mem.vif1_regs.mark, 0x1357u,
                         "MARK after fragmented V4-16 must run exactly once");
            }

            PS2Memory mem;
            PS2Memory contiguous;
            t.IsTrue(mem.initialize(), "fill-cycle fixture should initialize");
            t.IsTrue(contiguous.initialize(), "fill-cycle reference should initialize");
            for (PS2Memory *target : {&mem, &contiguous})
            {
                target->vif1_regs.cycle = (3u << 8) | 1u; // CL=1, WL=3
                target->vif1_regs.mask = 0x55555555u;
                for (uint32_t i = 0u; i < 4u; ++i)
                    target->vif1_regs.row[i] = 10u + i;
            }
            const uint32_t blocks[][4] = {
                {0u, 0u, makeVifCmd(0x7Cu, 3u, 0u), 0xAAAABBBBu},
                {0xCCCCDDDDu, 0xEEEEFFFFu, 0x12345678u,
                 makeVifCmd(0x07u, 0u, 0x2468u)}
            };
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(blocks[0]), 16u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(blocks[1]), 16u);
            contiguous.processVIF1Data(reinterpret_cast<const uint8_t *>(blocks),
                                       sizeof(blocks));
            t.IsTrue(std::memcmp(mem.getVU1Data(), contiguous.getVU1Data(),
                                 PS2_VU1_DATA_SIZE) == 0,
                     "fill-cycle UNPACK must preserve masked row writes when fragmented");
            for (uint32_t vec = 0u; vec < 3u; ++vec)
            {
                uint32_t x = 0u;
                std::memcpy(&x, mem.getVU1Data() + vec * 16u, 4u);
                t.Equals(x, 10u, "STCYCL fill must write the configured row value");
            }
            t.Equals(mem.vif1_regs.mark, 0x2468u,
                     "trailing MARK must execute after the fill-cycle UNPACK");
        });

        tc.Run("VIF1 reset discards an incomplete UNPACK payload", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "UNPACK reset fixture should initialize");
            const uint32_t partial[] = {
                makeVifCmd(0x6Cu, 2u, 0u), 1u, 2u, 3u
            };
            const uint32_t following[] = {
                makeVifCmd(0x07u, 0u, 0x4321u), 0u, 0u, 0u
            };
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(partial), 16u);
            t.IsTrue(mem.writeIORegister(0x10003C10u, 1u),
                     "FBRST.RST should discard pending UNPACK state");
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(following), 16u);
            t.Equals(mem.vif1_regs.mark, 0x4321u,
                     "post-reset MARK must not be consumed as the old UNPACK payload");
            uint32_t untouched = 0xFFFFFFFFu;
            std::memcpy(&untouched, mem.getVU1Data(), 4u);
            t.Equals(untouched, 0u, "cancelled UNPACK must leave VU1 memory untouched");
        });

        tc.Run("VIF UNPACK V4-16 sign and zero extension follow immediate bit14", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::memset(mem.getVU1Data(), 0, PS2_VU1_DATA_SIZE);

            // UNPACK V4-16 (opcode 0x6D), num=1, addr=0.
            // Payload components: x=0xFF80, y=0x0001, z=0x7FFF, w=0x8001.
            const uint16_t comps[4] = {0xFF80u, 0x0001u, 0x7FFFu, 0x8001u};

            std::vector<uint8_t> signPacket;
            appendU32(signPacket, makeVifCmd(0x6Du, 1u, 0x0000u)); // sign-extend
            for (uint16_t c : comps)
            {
                const size_t pos = signPacket.size();
                signPacket.resize(pos + sizeof(uint16_t));
                std::memcpy(signPacket.data() + pos, &c, sizeof(uint16_t));
            }
            mem.processVIF1Data(signPacket.data(), static_cast<uint32_t>(signPacket.size()));

            const uint8_t *vu1 = mem.getVU1Data();
            uint32_t sx = 0, sy = 0, sz = 0, sw = 0;
            std::memcpy(&sx, vu1 + 0, 4);
            std::memcpy(&sy, vu1 + 4, 4);
            std::memcpy(&sz, vu1 + 8, 4);
            std::memcpy(&sw, vu1 + 12, 4);
            t.Equals(sx, 0xFFFFFF80u, "sign-extend x");
            t.Equals(sy, 0x00000001u, "sign-extend y");
            t.Equals(sz, 0x00007FFFu, "sign-extend z");
            t.Equals(sw, 0xFFFF8001u, "sign-extend w");

            // Same UNPACK with imm bit14 set => zero-extend.
            std::vector<uint8_t> zeroPacket;
            appendU32(zeroPacket, makeVifCmd(0x6Du, 1u, 0x4000u)); // zero-extend
            for (uint16_t c : comps)
            {
                const size_t pos = zeroPacket.size();
                zeroPacket.resize(pos + sizeof(uint16_t));
                std::memcpy(zeroPacket.data() + pos, &c, sizeof(uint16_t));
            }
            mem.processVIF1Data(zeroPacket.data(), static_cast<uint32_t>(zeroPacket.size()));

            std::memcpy(&sx, vu1 + 0, 4);
            std::memcpy(&sy, vu1 + 4, 4);
            std::memcpy(&sz, vu1 + 8, 4);
            std::memcpy(&sw, vu1 + 12, 4);
            t.Equals(sx, 0x0000FF80u, "zero-extend x");
            t.Equals(sy, 0x00000001u, "zero-extend y");
            t.Equals(sz, 0x00007FFFu, "zero-extend z");
            t.Equals(sw, 0x00008001u, "zero-extend w");
        });

        tc.Run("VIF UNPACK bit15 adds TOPS to destination address", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::memset(mem.getVU1Data(), 0, PS2_VU1_DATA_SIZE);

            mem.vif1_regs.tops = 4u;

            // UNPACK V4-32, num=1, addr=2, bit15 set => effective addr = 6.
            std::vector<uint8_t> packet;
            appendU32(packet, makeVifCmd(0x6Cu, 1u, static_cast<uint16_t>(0x8000u | 0x0002u)));
            appendU32(packet, 0x11111111u);
            appendU32(packet, 0x22222222u);
            appendU32(packet, 0x33333333u);
            appendU32(packet, 0x44444444u);

            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            const uint8_t *vu1 = mem.getVU1Data();

            uint32_t untouched = 0xDEADBEEFu;
            std::memcpy(&untouched, vu1 + (2u * 16u), 4);
            t.Equals(untouched, 0u, "base addr without TOPS should remain untouched");

            uint32_t x = 0, y = 0, z = 0, w = 0;
            const uint32_t dest = 6u * 16u;
            std::memcpy(&x, vu1 + dest + 0u, 4);
            std::memcpy(&y, vu1 + dest + 4u, 4);
            std::memcpy(&z, vu1 + dest + 8u, 4);
            std::memcpy(&w, vu1 + dest + 12u, 4);
            t.Equals(x, 0x11111111u, "TOPS-adjusted x");
            t.Equals(y, 0x22222222u, "TOPS-adjusted y");
            t.Equals(z, 0x33333333u, "TOPS-adjusted z");
            t.Equals(w, 0x44444444u, "TOPS-adjusted w");
        });

        tc.Run("VIF STCYCL skip mode advances destination by CL when CL>=WL", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::memset(mem.getVU1Data(), 0, PS2_VU1_DATA_SIZE);

            std::vector<uint8_t> packet;
            appendU32(packet, makeVifCmd(0x01u, 0u, static_cast<uint16_t>((1u << 8) | 3u))); // STCYCL: WL=1, CL=3
            appendU32(packet, makeVifCmd(0x6Cu, 2u, 0u)); // UNPACK V4-32, NUM=2, ADDR=0

            appendU32(packet, 0x11111111u);
            appendU32(packet, 0x22222222u);
            appendU32(packet, 0x33333333u);
            appendU32(packet, 0x44444444u);

            appendU32(packet, 0xAAAAAAAAu);
            appendU32(packet, 0xBBBBBBBBu);
            appendU32(packet, 0xCCCCCCCCu);
            appendU32(packet, 0xDDDDDDDDu);

            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            const uint8_t *vu = mem.getVU1Data();

            uint32_t v0x = 0, v1x = 0, v2x = 0, v3x = 0;
            std::memcpy(&v0x, vu + 0u * 16u + 0u, 4);
            std::memcpy(&v1x, vu + 1u * 16u + 0u, 4);
            std::memcpy(&v2x, vu + 2u * 16u + 0u, 4);
            std::memcpy(&v3x, vu + 3u * 16u + 0u, 4);

            t.Equals(v0x, 0x11111111u, "first vector should write at addr 0");
            t.Equals(v1x, 0u, "skip mode should leave addr 1 untouched when WL=1 CL=3");
            t.Equals(v2x, 0u, "skip mode should leave addr 2 untouched when WL=1 CL=3");
            t.Equals(v3x, 0xAAAAAAAAu, "second vector should write at addr CL (addr 3)");
        });

        tc.Run("VIF masked UNPACK uses data row col and protect selectors", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::memset(mem.getVU1Data(), 0, PS2_VU1_DATA_SIZE);

            // Pre-fill destination W lane for write-protect verification.
            uint32_t preservedW = 0xDEADBEEFu;
            std::memcpy(mem.getVU1Data() + 12u, &preservedW, 4u);

            std::vector<uint8_t> packet;
            appendU32(packet, makeVifCmd(0x20u, 0u, 0u)); // STMASK
            appendU32(packet, 0x000000E4u); // m0=0(data), m1=1(row), m2=2(col), m3=3(protect)

            appendU32(packet, makeVifCmd(0x30u, 0u, 0u)); // STROW
            appendU32(packet, 0xAAAAB001u);
            appendU32(packet, 0xAAAAB002u);
            appendU32(packet, 0xAAAAB003u);
            appendU32(packet, 0xAAAAB004u);

            appendU32(packet, makeVifCmd(0x31u, 0u, 0u)); // STCOL
            appendU32(packet, 0x11110001u);
            appendU32(packet, 0x11110002u);
            appendU32(packet, 0x11110003u);
            appendU32(packet, 0x11110004u);

            appendU32(packet, makeVifCmd(0x7Cu, 1u, 0u)); // UNPACK V4-32 with CMD bit4 (mask enable)
            appendU32(packet, 0x01020304u);
            appendU32(packet, 0x11121314u);
            appendU32(packet, 0x21222324u);
            appendU32(packet, 0x31323334u);

            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            const uint8_t *vu = mem.getVU1Data();
            uint32_t x = 0, y = 0, z = 0, w = 0;
            std::memcpy(&x, vu + 0u, 4u);
            std::memcpy(&y, vu + 4u, 4u);
            std::memcpy(&z, vu + 8u, 4u);
            std::memcpy(&w, vu + 12u, 4u);

            t.Equals(x, 0x01020304u, "mask=0 should write decompressed data");
            t.Equals(y, 0xAAAAB002u, "mask=1 should write row register for Y field");
            t.Equals(z, 0x11110001u, "mask=2 should write C0 on first write cycle");
            t.Equals(w, preservedW, "mask=3 should write-protect destination field");
        });

        tc.Run("VIF STMOD offset and difference modes apply to UNPACK data", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::memset(mem.getVU1Data(), 0, PS2_VU1_DATA_SIZE);

            std::vector<uint8_t> packet;
            appendU32(packet, makeVifCmd(0x30u, 0u, 0u)); // STROW
            appendU32(packet, 10u);
            appendU32(packet, 20u);
            appendU32(packet, 30u);
            appendU32(packet, 40u);

            appendU32(packet, makeVifCmd(0x05u, 0u, 1u)); // STMOD offset mode
            appendU32(packet, makeVifCmd(0x6Cu, 1u, 0u)); // UNPACK V4-32 -> addr 0
            appendU32(packet, 1u);
            appendU32(packet, 2u);
            appendU32(packet, 3u);
            appendU32(packet, 4u);

            appendU32(packet, makeVifCmd(0x30u, 0u, 0u)); // reset STROW for difference mode
            appendU32(packet, 100u);
            appendU32(packet, 100u);
            appendU32(packet, 100u);
            appendU32(packet, 100u);

            appendU32(packet, makeVifCmd(0x05u, 0u, 2u)); // STMOD difference mode
            appendU32(packet, makeVifCmd(0x6Cu, 2u, 1u)); // UNPACK V4-32 -> addr 1 and 2
            appendU32(packet, 1u);
            appendU32(packet, 1u);
            appendU32(packet, 1u);
            appendU32(packet, 1u);
            appendU32(packet, 2u);
            appendU32(packet, 2u);
            appendU32(packet, 2u);
            appendU32(packet, 2u);

            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            const uint8_t *vu = mem.getVU1Data();
            uint32_t x0 = 0, y0 = 0, z0 = 0, w0 = 0;
            std::memcpy(&x0, vu + 0u * 16u + 0u, 4u);
            std::memcpy(&y0, vu + 0u * 16u + 4u, 4u);
            std::memcpy(&z0, vu + 0u * 16u + 8u, 4u);
            std::memcpy(&w0, vu + 0u * 16u + 12u, 4u);
            t.Equals(x0, 11u, "offset mode X");
            t.Equals(y0, 22u, "offset mode Y");
            t.Equals(z0, 33u, "offset mode Z");
            t.Equals(w0, 44u, "offset mode W");

            uint32_t x1 = 0, x2 = 0;
            std::memcpy(&x1, vu + 1u * 16u + 0u, 4u);
            std::memcpy(&x2, vu + 2u * 16u + 0u, 4u);
            t.Equals(x1, 101u, "difference mode first write should add initial row");
            t.Equals(x2, 103u, "difference mode second write should accumulate updated row");
            t.Equals(mem.vif1_regs.row[0], 103u, "difference mode should update row register");
            t.Equals(mem.vif1_regs.row[1], 103u, "difference mode should update row register for Y");
            t.Equals(mem.vif1_regs.row[2], 103u, "difference mode should update row register for Z");
            t.Equals(mem.vif1_regs.row[3], 103u, "difference mode should update row register for W");
        });

        tc.Run("VIF fill write uses STMASK and STROW when WL>CL", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::memset(mem.getVU1Data(), 0, PS2_VU1_DATA_SIZE);

            std::vector<uint8_t> packet;
            appendU32(packet, makeVifCmd(0x01u, 0u, static_cast<uint16_t>((3u << 8) | 1u))); // STCYCL: WL=3, CL=1

            appendU32(packet, makeVifCmd(0x20u, 0u, 0u)); // STMASK
            appendU32(packet, 0x55555555u); // all fields all cycles use row register

            appendU32(packet, makeVifCmd(0x30u, 0u, 0u)); // STROW
            appendU32(packet, 0x11111111u);
            appendU32(packet, 0x22222222u);
            appendU32(packet, 0x33333333u);
            appendU32(packet, 0x44444444u);

            appendU32(packet, makeVifCmd(0x7Cu, 3u, 0u)); // masked UNPACK V4-32, NUM=3 writes
            // Only one input vector should be consumed for CL=1, WL=3.
            appendU32(packet, 0xAAAABBBB);
            appendU32(packet, 0xCCCCDDDD);
            appendU32(packet, 0xEEEEFFFF);
            appendU32(packet, 0x12345678);

            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            const uint8_t *vu = mem.getVU1Data();
            for (uint32_t i = 0; i < 3u; ++i)
            {
                uint32_t x = 0, y = 0, z = 0, w = 0;
                std::memcpy(&x, vu + i * 16u + 0u, 4u);
                std::memcpy(&y, vu + i * 16u + 4u, 4u);
                std::memcpy(&z, vu + i * 16u + 8u, 4u);
                std::memcpy(&w, vu + i * 16u + 12u, 4u);
                t.Equals(x, 0x11111111u, "fill write X should use row[0]");
                t.Equals(y, 0x22222222u, "fill write Y should use row[1]");
                t.Equals(z, 0x33333333u, "fill write Z should use row[2]");
                t.Equals(w, 0x44444444u, "fill write W should use row[3]");
            }
        });

        tc.Run("VIF1_STAT writes affect only FIFO direction", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "VIF1 STAT write fixture should initialize");
            constexpr uint32_t kStat = 0x10003C00u;
            constexpr uint32_t kFbrst = 0x10003C10u;
            constexpr uint32_t kFdr = 1u << 23u;
            constexpr uint32_t kInt = 1u << 11u;
            constexpr uint32_t kMrk = 1u << 6u;
            constexpr uint32_t kDbf = 1u << 7u;

            const uint32_t markI = makeVifCmd(0x07u, 0u, 0x1357u) | 0x80000000u;
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&markI),
                                sizeof(markI));
            t.Equals(mem.read32(kStat) & (kInt | kMrk), kInt | kMrk,
                     "MARK with an I-bit should publish interrupt and marker flags");
            // Set the hardware-controlled DBF flag to test its write mask.
            mem.vif1_regs.stat |= kDbf;
            const uint32_t prior = mem.read32(kStat);

            t.IsTrue(mem.writeIORegister(kStat, 0xFFFFFFFFu),
                     "VIF1_STAT must accept guest register stores");
            t.Equals(mem.read32(kStat), prior | kFdr,
                     "all-one write must set FDR without modifying read-only flags");

            t.IsTrue(mem.writeIORegister(kStat, kInt | kMrk),
                     "write with read-only flag bits must be accepted");
            t.Equals(mem.read32(kStat), prior,
                     "FDR zero must clear direction while preserving INT, MRK and DBF");

            mem.write32(kStat, kFdr);
            t.Equals(mem.read32(kStat), prior | kFdr,
                     "standard MMIO write32 must update the live FDR flag");
            mem.write8(kStat, 0u);
            t.Equals(mem.read32(kStat), prior | kFdr,
                     "a low-byte write must not clear unrelated high-byte FDR");
            mem.write16(kStat + 2u, 0u);
            t.Equals(mem.read32(kStat), prior,
                     "an upper halfword write must update the FDR bit");

            mem.write32(kStat, kFdr);
            mem.write32(kFbrst, 0x8u);
            t.Equals(mem.read32(kStat), (prior & ~kInt) | kFdr,
                     "FBRST.STC must acknowledge INT without clearing FIFO direction");
            mem.write32(kFbrst, 1u);
            t.Equals(mem.read32(kStat), 0u,
                     "FBRST.RST must reset FIFO direction and all status flags");
        });

        tc.Run("VIF irq command sets STAT.INT and CODE until FBRST.STC clears it", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            const uint32_t irqMarkCmd = 0x80000000u | makeVifCmd(0x07u, 0x12u, 0x3456u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&irqMarkCmd), sizeof(irqMarkCmd));

            t.Equals(mem.vif1_regs.code, irqMarkCmd, "VIF CODE should capture the last processed command");
            t.IsTrue((mem.vif1_regs.stat & (1u << 11)) != 0u, "irq bit should raise VIF1 STAT.INT");
            t.Equals(mem.vif1_regs.mark, 0x3456u, "MARK command should still update MARK register");

            t.IsTrue(mem.writeIORegister(0x10003C10u, 0x8u), "FBRST STC write should succeed");
            t.IsTrue((mem.vif1_regs.stat & (1u << 11)) == 0u, "FBRST.STC should clear VIF1 STAT.INT");
        });

        tc.Run("VIF FBRST RST clears VIF1 command state", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            mem.vif1_regs.mark = 0x1234u;
            mem.vif1_regs.cycle = 0x0102u;
            mem.vif1_regs.mode = 2u;
            mem.vif1_regs.num = 7u;
            mem.vif1_regs.mask = 0x89ABCDEFu;
            mem.vif1_regs.code = 0xCAFEBABEu;
            mem.vif1_regs.stat = 0x3F00u;

            t.IsTrue(mem.writeIORegister(0x10003C10u, 0x1u), "FBRST RST write should succeed");

            t.Equals(mem.vif1_regs.mark, 0u, "RST should clear MARK");
            t.Equals(mem.vif1_regs.cycle, 0u, "RST should clear CYCLE");
            t.Equals(mem.vif1_regs.mode, 0u, "RST should clear MODE");
            t.Equals(mem.vif1_regs.num, 0u, "RST should clear NUM");
            t.Equals(mem.vif1_regs.mask, 0u, "RST should clear MASK");
            t.Equals(mem.vif1_regs.code, 0u, "RST should clear CODE");
            t.Equals(mem.vif1_regs.stat, 0u, "RST should clear STAT");
        });

        tc.Run("VIF double-buffer OFFSET BASE and MSCAL update TOPS and ITOPS", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            mem.vif1_regs.base = 0x120u;
            mem.vif1_regs.tops = 0x120u;
            mem.vif1_regs.stat = (1u << 7); // DBF=1 before OFFSET

            struct MscalCall
            {
                uint32_t startPC;
                uint32_t top;
                uint32_t itop;
            };
            std::vector<MscalCall> mscalCalls;
            mem.setVu1MscalCallback([&](uint32_t startPC, uint32_t top, uint32_t itop)
            {
                mscalCalls.push_back({startPC, top, itop});
            });

            const uint32_t offsetCmd = makeVifCmd(0x02u, 0u, 0x0022u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&offsetCmd), sizeof(offsetCmd));
            t.Equals(mem.vif1_regs.ofst, 0x22u, "OFFSET should update OFST");
            t.Equals(mem.vif1_regs.base, 0x120u, "OFFSET should copy old TOPS into BASE");
            t.IsTrue((mem.vif1_regs.stat & (1u << 7)) == 0u, "OFFSET should clear DBF");
            t.Equals(mem.vif1_regs.tops, 0x120u, "DBF=0 should keep TOPS at BASE");

            const uint32_t baseCmd = makeVifCmd(0x03u, 0u, 0x0030u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&baseCmd), sizeof(baseCmd));
            t.Equals(mem.vif1_regs.base, 0x30u, "BASE should update BASE register");
            t.Equals(mem.vif1_regs.tops, 0x120u, "BASE should not rewrite current TOPS");

            const uint32_t itopCmd = makeVifCmd(0x04u, 0u, 0x0044u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&itopCmd), sizeof(itopCmd));
            t.Equals(mem.vif1_regs.itops, 0x44u, "ITOP VIFcode should update pending ITOPS register");

            const uint32_t mscalCmd = makeVifCmd(0x14u, 0u, 0x0003u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&mscalCmd), sizeof(mscalCmd));
            t.Equals(mscalCalls.size(), static_cast<size_t>(1u), "MSCAL should invoke callback once");
            t.Equals(mscalCalls[0].startPC, 0x18u, "MSCAL callback startPC should be IMMEDIATE*8");
            t.Equals(mscalCalls[0].top, 0x120u, "MSCAL callback should receive current TOPS");
            t.Equals(mscalCalls[0].itop, 0x44u, "MSCAL callback should receive pending ITOPS");
            t.Equals(mem.vif1_regs.top, 0x120u, "MSCAL should latch TOP from TOPS");
            t.Equals(mem.vif1_regs.itop, 0x44u, "MSCAL should latch ITOP from ITOPS");
            t.IsTrue((mem.vif1_regs.stat & (1u << 7)) != 0u, "MSCAL should toggle DBF");
            t.Equals(mem.vif1_regs.tops, 0x52u, "DBF=1 should set TOPS to BASE+OFST");

            const uint32_t mscntCmd = makeVifCmd(0x17u, 0u, 0u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&mscntCmd), sizeof(mscntCmd));
            t.IsTrue((mem.vif1_regs.stat & (1u << 7)) == 0u, "MSCNT should toggle DBF again");
            t.Equals(mem.vif1_regs.tops, 0x30u, "DBF=0 should restore TOPS to BASE");
        });

        tc.Run("VIF MSKPATH3 uses immediate bit15", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            const uint32_t setMask = makeVifCmd(0x06u, 0u, 0x8000u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&setMask), sizeof(setMask));
            t.IsTrue(mem.isPath3Masked(), "MSKPATH3 with imm bit15 set should enable PATH3 mask");

            const uint32_t clearMask = makeVifCmd(0x06u, 0u, 0x0000u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&clearMask), sizeof(clearMask));
            t.IsFalse(mem.isPath3Masked(), "MSKPATH3 with imm bit15 clear should disable PATH3 mask");
        });

        tc.Run("VIF1 FIFO MSKPATH3 is visible through GIF_STAT", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            const uint32_t setMask = makeVifCmd(0x06u, 0u, 0x8000u);
            const __m128i setPacket = _mm_set_epi32(0, 0, 0, static_cast<int32_t>(setMask));
            mem.write128(0x10005000u, setPacket);
            t.IsTrue(mem.isPath3Masked(), "a direct VIF1 FIFO command should execute MSKPATH3");
            t.IsTrue((mem.readIORegister(0x10003020u) & 0x2u) != 0u,
                     "GIF_STAT.M3P should report the VIF1 PATH3 mask");

            const uint32_t clearMask = makeVifCmd(0x06u, 0u, 0x0000u);
            const __m128i clearPacket = _mm_set_epi32(0, 0, 0, static_cast<int32_t>(clearMask));
            mem.write128(0x10005000u, clearPacket);
            t.IsFalse(mem.isPath3Masked(), "a direct VIF1 FIFO command should clear MSKPATH3");
            t.IsTrue((mem.readIORegister(0x10003020u) & 0x2u) == 0u,
                     "GIF_STAT.M3P should clear with the VIF1 PATH3 mask");

            mem.writeIORegister(0x10003010u, 0x5u); // GIF_MODE: M3R | IMT
            mem.writeIORegister(0x10003000u, 0x8u); // GIF_CTRL: PSE
            t.Equals(mem.readIORegister(0x10003020u) & 0xDu, 0xDu,
                     "GIF_STAT should mirror GIF_MODE and GIF_CTRL status bits");
        });

        tc.Run("GIF_STAT exposes synchronously drained DMA occupancy for one EE quantum", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kGifChannel = 0x1000A000u;
            constexpr uint32_t kSource = 0x00020000u;
            mem.setGifPacketCallback([](const uint8_t *, uint32_t) {});

            t.IsTrue(mem.writeIORegister(kGifChannel + 0x10u, kSource),
                     "write GIF MADR should succeed");
            t.IsTrue(mem.writeIORegister(kGifChannel + 0x20u, 1u),
                     "write GIF QWC should succeed");
            t.IsTrue(mem.writeIORegister(kGifChannel + 0x00u, 0x100u),
                     "start GIF normal DMA should succeed");

            const uint32_t visibleFqc = (mem.readIORegister(0x10003020u) >> 24u) & 0x1Fu;
            t.Equals(visibleFqc, 1u,
                     "a synchronously consumed qword should remain observable through GIF_STAT.FQC");

            mem.advanceEeTimers(1u);
            const uint32_t drainedFqc = (mem.readIORegister(0x10003020u) >> 24u) & 0x1Fu;
            t.Equals(drainedFqc, 0u,
                     "the synthetic FIFO observation should expire at the next EE scheduling boundary");
        });

        tc.Run("PATH3 mask queues packets until unmask", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                captured.emplace_back(data, data + sizeBytes);
            });

            std::vector<uint8_t> packetA(16u);
            std::vector<uint8_t> packetB(16u);
            for (uint32_t i = 0; i < 16u; ++i)
            {
                packetA[i] = static_cast<uint8_t>(0x10u + i);
                packetB[i] = static_cast<uint8_t>(0x40u + i);
            }

            const uint32_t setMask = makeVifCmd(0x06u, 0u, 0x8000u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&setMask), sizeof(setMask));
            t.IsTrue(mem.isPath3Masked(), "PATH3 mask should be enabled");

            mem.submitGifPacket(GifPathId::Path3, packetA.data(), static_cast<uint32_t>(packetA.size()));
            mem.submitGifPacket(GifPathId::Path3, packetB.data(), static_cast<uint32_t>(packetB.size()));
            t.Equals(captured.size(), static_cast<size_t>(0u), "masked PATH3 packets should be queued, not dropped/emitted");

            const uint32_t clearMask = makeVifCmd(0x06u, 0u, 0x0000u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&clearMask), sizeof(clearMask));

            t.Equals(captured.size(), static_cast<size_t>(2u), "unmask should flush queued PATH3 packets");
            bool firstOk = true;
            bool secondOk = true;
            for (uint32_t i = 0; i < 16u; ++i)
            {
                if (captured[0][i] != static_cast<uint8_t>(0x10u + i))
                    firstOk = false;
                if (captured[1][i] != static_cast<uint8_t>(0x40u + i))
                    secondOk = false;
            }
            t.IsTrue(firstOk, "first queued PATH3 packet should flush in-order");
            t.IsTrue(secondOk, "second queued PATH3 packet should flush in-order");
        });

        tc.Run("GIF arbiter prioritizes PATH1 then PATH2 then PATH3", [](TestCase &t)
        {
            std::vector<uint8_t> order;
            GifArbiter arbiter([&](const uint8_t *data, uint32_t sizeBytes)
            {
                if (data && sizeBytes > 0u)
                    order.push_back(data[0]);
            });

            const std::vector<uint8_t> p1(16u, 0x11u);
            const std::vector<uint8_t> p2(16u, 0x22u);
            const std::vector<uint8_t> p3(16u, 0x33u);

            arbiter.submit(GifPathId::Path3, p3.data(), static_cast<uint32_t>(p3.size()));
            arbiter.submit(GifPathId::Path2, p2.data(), static_cast<uint32_t>(p2.size()));
            arbiter.submit(GifPathId::Path1, p1.data(), static_cast<uint32_t>(p1.size()));
            arbiter.drain();

            t.Equals(order.size(), static_cast<size_t>(3u), "all queued packets should be drained");
            t.Equals(order[0], static_cast<uint8_t>(0x11u), "PATH1 should be drained first");
            t.Equals(order[1], static_cast<uint8_t>(0x22u), "PATH2 should be drained second");
            t.Equals(order[2], static_cast<uint8_t>(0x33u), "PATH3 should be drained third");
        });

        tc.Run("VIF1 DMA resumes split DIRECT and DIRECTHL payloads", [](TestCase &t)
        {
            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kSource = 0x0002B000u;
            for (const uint8_t opcode : {uint8_t{0x50u}, uint8_t{0x51u}})
            {
                PS2Memory mem;
                t.IsTrue(mem.initialize(), "split DIRECT DMA fixture should initialize");
                std::vector<std::vector<uint8_t>> packets;
                mem.setGifPacketCallback([&](const uint8_t *data, uint32_t count)
                {
                    packets.emplace_back(data, data + count);
                });

                const uint32_t chunks[][4] = {
                    {0u, 0u, 0u, makeVifCmd(opcode, 0u, 2u)},
                    {0x00000001u, 0x08000000u, 0u, 0u},
                    {0x11223344u, 0x55667788u, 0x99AABBCCu, 0xDDEEFF00u},
                    {makeVifCmd(0x07u, 0u, 0x1234u), 0u, 0u, 0u}
                };
                // First fragment ends exactly at the VIFcode. The next two
                // DMA starts contain one GIFtag and one IMAGE qword.
                for (uint32_t chunk = 0u; chunk < 4u; ++chunk)
                {
                    std::memcpy(mem.getRDRAM() + kSource, chunks[chunk], 16u);
                    mem.writeIORegister(kVif1Ch + 0x10u, kSource);
                    mem.writeIORegister(kVif1Ch + 0x20u, 1u);
                    mem.writeIORegister(kVif1Ch, 0x100u);
                    if (chunk < 2u)
                        t.Equals(packets.size(), static_cast<size_t>(0u),
                                 "incomplete DIRECT must not dispatch a partial GIF packet");
                }

                t.Equals(packets.size(), static_cast<size_t>(1u),
                         "split DIRECT must dispatch the complete GIF packet once");
                if (packets.size() == 1u)
                {
                    t.Equals(packets[0].size(), static_cast<size_t>(32u),
                             "completed DIRECT payload must include two GIF quadwords");
                    if (packets[0].size() == 32u)
                    {
                        bool bytesMatch = true;
                        const auto *expected = reinterpret_cast<const uint8_t *>(&chunks[1][0]);
                        for (uint32_t i = 0u; i < 16u; ++i)
                        {
                            if (packets[0][i] != expected[i] ||
                                packets[0][i + 16u] !=
                                    reinterpret_cast<const uint8_t *>(&chunks[2][0])[i])
                            {
                                bytesMatch = false;
                                break;
                            }
                        }
                        t.IsTrue(bytesMatch, "DIRECT payload bytes must retain transport order");
                    }
                }
                t.Equals(mem.vif1_regs.mark, 0x1234u,
                         "VIFcodes after a complete DIRECT should resume normally");
                t.Equals(mem.readIORegister(kVif1Ch + 0x20u), 0u,
                         "each completed VIF1 DMA must leave QWC zero");
            }
        });

        tc.Run("VIF1 DIRECT resumes inside a GIF quadword before trailing VIFcodes", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "unaligned DIRECT fixture should initialize");
            std::vector<std::vector<uint8_t>> packets;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t count)
            {
                packets.emplace_back(data, data + count);
            });
            const uint32_t first[] = {
                makeVifCmd(0x50u, 0u, 1u), 0x11111111u, 0x22222222u, 0x33333333u
            };
            const uint32_t second[] = {
                0x44444444u, makeVifCmd(0x07u, 0u, 0x5678u), 0u, 0u
            };
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(first), sizeof(first));
            t.Equals(packets.size(), static_cast<size_t>(0u),
                     "twelve-byte DIRECT fragment must not be emitted prematurely");
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(second), sizeof(second));
            t.Equals(packets.size(), static_cast<size_t>(1u),
                     "remaining four bytes should complete one GIF quadword");
            if (packets.size() == 1u)
            {
                t.Equals(packets[0].size(), static_cast<size_t>(16u),
                         "unaligned DIRECT must emit exactly one quadword");
                uint32_t words[4]{};
                if (packets[0].size() == 16u)
                    std::memcpy(words, packets[0].data(), sizeof words);
                t.Equals(words[0], 0x11111111u, "first GIF word must be preserved");
                t.Equals(words[1], 0x22222222u, "second GIF word must be preserved");
                t.Equals(words[2], 0x33333333u, "third GIF word must be preserved");
                t.Equals(words[3], 0x44444444u, "fourth GIF word must cross the boundary");
            }
            t.Equals(mem.vif1_regs.mark, 0x5678u,
                     "next VIFcode after a split quadword must be decoded");
        });

        tc.Run("VIF1 FBRST cancels a fragmented DIRECT command", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "DIRECT reset fixture should initialize");
            std::vector<std::vector<uint8_t>> packets;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t count)
            {
                packets.emplace_back(data, data + count);
            });
            const uint32_t partial[] = {
                makeVifCmd(0x51u, 0u, 2u), 0xAAAA0001u, 0xAAAA0002u, 0xAAAA0003u
            };
            const uint32_t following[] = {
                makeVifCmd(0x07u, 0u, 0xABCDu), 0u, 0u, 0u
            };
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(partial), sizeof(partial));
            t.IsTrue(mem.writeIORegister(0x10003C10u, 1u),
                     "VIF1 reset should accept FBRST.RST");
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(following), sizeof(following));
            t.Equals(mem.vif1_regs.mark, 0xABCDu,
                     "a reset must cancel the pending DIRECT payload");
            t.Equals(packets.size(), static_cast<size_t>(0u),
                     "an abandoned DIRECT must not submit a partial GIF packet");
        });

        tc.Run("VIF DIRECTHL stalls behind queued PATH3 IMAGE packets", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            std::vector<uint8_t> firstBytes;
            GifArbiter arbiter([&](const uint8_t *data, uint32_t sizeBytes)
            {
                if (data && sizeBytes > 0u)
                    firstBytes.push_back(data[0]);
            });
            mem.setGifArbiter(&arbiter);

            std::vector<uint8_t> path3Image;
            appendU64(path3Image, makeGifTag(0x00AAu, 2u, 0u, true)); // IMAGE packet marker: first byte 0xAA
            appendU64(path3Image, 0ull);
            mem.submitGifPacket(GifPathId::Path3, path3Image.data(), static_cast<uint32_t>(path3Image.size()), false);

            std::vector<uint8_t> vifPacket;
            appendU32(vifPacket, makeVifCmd(0x51u, 0u, 1u)); // DIRECTHL 1 QW
            for (uint32_t i = 0; i < 16u; ++i)
            {
                vifPacket.push_back(static_cast<uint8_t>(0xD2u + i));
            }
            mem.processVIF1Data(vifPacket.data(), static_cast<uint32_t>(vifPacket.size()));

            t.Equals(firstBytes.size(), static_cast<size_t>(2u), "PATH3 and DIRECTHL packets should both drain");
            t.Equals(firstBytes[0], static_cast<uint8_t>(0xAAu), "DIRECTHL should not preempt queued PATH3 IMAGE packet");
            t.Equals(firstBytes[1], static_cast<uint8_t>(0xD2u), "DIRECTHL packet should drain after PATH3 IMAGE packet");
        });

        tc.Run("GIF DMA mode0 copies RDRAM packet and clears channel", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kGifCh = 0x1000A000u;
            constexpr uint32_t kSrc = 0x00022000u;
            constexpr uint32_t kQwc = 2u; // 32 bytes

            uint8_t *rdram = mem.getRDRAM();
            for (uint32_t i = 0; i < kQwc * 16u; ++i)
            {
                rdram[kSrc + i] = static_cast<uint8_t>((0x40u + i) & 0xFFu);
            }

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                captured.emplace_back(data, data + sizeBytes);
            });

            t.IsTrue(mem.writeIORegister(kGifCh + 0x10u, kSrc), "write MADR should succeed");
            t.IsTrue(mem.writeIORegister(kGifCh + 0x20u, kQwc), "write QWC should succeed");
            t.IsTrue(mem.writeIORegister(kGifCh + 0x00u, 0x100u), "write CHCR STR should succeed");

            t.Equals(mem.dmaStartCount(), 1ull, "starting GIF DMA should increment dmaStartCount");

            mem.processPendingTransfers();

            t.Equals(captured.size(), static_cast<size_t>(1u), "GIF DMA should emit one packet");
            t.Equals(captured[0].size(), static_cast<size_t>(kQwc * 16u), "GIF packet size should match QWC");

            bool contentOk = true;
            for (uint32_t i = 0; i < kQwc * 16u; ++i)
            {
                if (captured[0][i] != static_cast<uint8_t>((0x40u + i) & 0xFFu))
                {
                    contentOk = false;
                    break;
                }
            }
            t.IsTrue(contentOk, "GIF DMA packet bytes should match source RDRAM");
            t.IsTrue(mem.hasSeenGifCopy(), "GIF DMA should mark seen GIF copy");
            t.Equals(mem.gifCopyCount(), 1ull, "GIF DMA should increment gifCopyCount");
            t.IsTrue((mem.readIORegister(kGifCh + 0x00u) & 0x100u) == 0u, "GIF CHCR STR bit should be cleared after drain");
            t.Equals(mem.readIORegister(kGifCh + 0x20u), 0u, "GIF QWC should be cleared after drain");
        });

        tc.Run("GIF DMA can source from scratchpad", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kGifCh = 0x1000A000u;
            constexpr uint32_t kSrcScratch = PS2_SCRATCHPAD_BASE + 0x80u;
            constexpr uint32_t kQwc = 1u; // 16 bytes

            uint8_t *scratch = mem.getScratchpad();
            for (uint32_t i = 0; i < 16u; ++i)
            {
                scratch[0x80u + i] = static_cast<uint8_t>((0xA0u + i) & 0xFFu);
            }

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                captured.emplace_back(data, data + sizeBytes);
            });

            t.IsTrue(mem.writeIORegister(kGifCh + 0x10u, kSrcScratch), "write MADR scratchpad should succeed");
            t.IsTrue(mem.writeIORegister(kGifCh + 0x20u, kQwc), "write QWC should succeed");
            t.IsTrue(mem.writeIORegister(kGifCh + 0x00u, 0x100u), "write CHCR STR should succeed");

            mem.processPendingTransfers();

            t.Equals(captured.size(), static_cast<size_t>(1u), "scratchpad GIF DMA should emit one packet");
            t.Equals(captured[0].size(), static_cast<size_t>(16u), "scratchpad GIF DMA packet should be 16 bytes");
            bool contentOk = true;
            for (uint32_t i = 0; i < 16u; ++i)
            {
                if (captured[0][i] != static_cast<uint8_t>((0xA0u + i) & 0xFFu))
                {
                    contentOk = false;
                    break;
                }
            }
            t.IsTrue(contentOk, "scratchpad GIF DMA packet bytes should match scratchpad source");
        });

        tc.Run("GIF DMA chain can source tags and payload from 0xF000 scratchpad alias", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kGifCh = 0x1000A000u;
            constexpr uint32_t kTagAlias = PS2_SCRATCHPAD_ALIAS_BASE + 0x100u;

            uint8_t *scratch = mem.getScratchpad();
            std::memset(scratch + 0x100u, 0, 32u);

            const uint64_t endTag = makeDmaTag(1u, 7u, 0u, false);
            std::memcpy(scratch + 0x100u, &endTag, sizeof(endTag));
            for (uint32_t i = 0; i < 16u; ++i)
            {
                scratch[0x110u + i] = static_cast<uint8_t>(0xC0u + i);
            }

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                captured.emplace_back(data, data + sizeBytes);
            });

            t.IsTrue(mem.writeIORegister(kGifCh + 0x30u, kTagAlias), "write TADR scratchpad alias should succeed");
            t.IsTrue(mem.writeIORegister(kGifCh + 0x00u, 0x104u), "write CHCR STR|CHAIN should succeed");

            mem.processPendingTransfers();

            t.Equals(captured.size(), static_cast<size_t>(1u), "scratchpad alias chain should emit one packet");
            t.Equals(captured[0].size(), static_cast<size_t>(16u), "scratchpad alias chain should emit one qword");

            bool contentOk = true;
            for (uint32_t i = 0; i < 16u; ++i)
            {
                if (captured[0][i] != static_cast<uint8_t>(0xC0u + i))
                {
                    contentOk = false;
                    break;
                }
            }
            t.IsTrue(contentOk, "scratchpad alias chain payload should match scratchpad bytes");
        });

        tc.Run("native GIF image upload recognizes canonical load-image chain", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            GS gs;
            gs.init(mem.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &mem.gs());

            constexpr uint32_t kGifCh = 0x1000A000u;
            constexpr uint32_t kDStat = 0x1000E010u;
            constexpr uint32_t kChain = 0x00028000u;
            constexpr uint32_t kPixels = 0x00029000u;
            constexpr uint32_t kQwc = 1u;

            uint8_t *rdram = mem.getRDRAM();
            for (uint32_t i = 0; i < kQwc * 16u; ++i)
            {
                rdram[kPixels + i] = static_cast<uint8_t>(0x40u + i);
            }

            uint32_t chain = kChain;
            chain = writeTextureUploadSetup(rdram, chain, 0u, GS_PSM_CT32);
            chain = writeTextureImageRef(rdram, chain, kQwc, kPixels);
            writeDmaTag(rdram, chain, makeDmaTag(0u, 7u, 0u, false)); // END.

            t.IsTrue(mem.writeIORegister(kGifCh + 0x30u, kChain), "write GIF TADR should succeed");
            t.IsTrue(mem.tryProcessNativeGifImageUploadChain(gs, kChain, 0x105u),
                     "canonical load-image chain should use the native upload path");

            t.Equals(gs.nativeImageUploadCount(), 1ull, "native GIF DMA chain should upload through GS fast path");
            t.Equals(mem.gifCopyCount(), 1ull, "native GIF DMA chain should still count as a GIF DMA copy");
            t.IsTrue((mem.readIORegister(kDStat) & (1u << 2u)) != 0u,
                     "native GIF DMA chain should raise D_STAT GIF completion");
            t.Equals(mem.readIORegister(kGifCh + 0x20u), 0u, "native GIF DMA chain should clear GIF QWC");
            t.Equals(mem.readIORegister(kGifCh + 0x00u) & 0x100u, 0u,
                     "native GIF DMA chain should clear GIF STR");
            t.Equals(mem.readIORegister(kGifCh + 0x00u) & 0x70000000u, 0x70000000u,
                     "native GIF DMA chain should latch the terminal END tag id");

            bool pixelsOk = true;
            for (uint32_t x = 0; x < 4u && pixelsOk; ++x)
            {
                const uint32_t dstOff = GSPSMCT32::addrPSMCT32(0u, 1u, x, 0u);
                const uint32_t srcOff = kPixels + x * 4u;
                for (uint32_t c = 0; c < 4u; ++c)
                {
                    if (mem.getGSVRAM()[dstOff + c] != rdram[srcOff + c])
                    {
                        pixelsOk = false;
                        break;
                    }
                }
            }
            t.IsTrue(pixelsOk, "native GIF DMA chain should upload image payload into GS VRAM");
        });

        tc.Run("native GIF packed chain matches generic packed primitive packet", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            GS nativeGs;
            nativeGs.init(mem.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &mem.gs());

            GSRegisters genericRegs{};
            std::vector<uint8_t> genericVram(PS2_GS_VRAM_SIZE, 0u);
            GS genericGs;
            genericGs.init(genericVram.data(), static_cast<uint32_t>(genericVram.size()), &genericRegs);

            std::vector<uint8_t> packet;
            appendU64(packet, makeGifTag(4u, GIF_FMT_PACKED, 1u, false));
            appendU64(packet, 0x0Eull);
            appendU64(packet, makeGsFrame(0u, 1u, GS_PSM_CT32));
            appendU64(packet, GS_REG_FRAME_1);
            appendU64(packet, makeGsScissor(0u, 7u, 0u, 7u));
            appendU64(packet, GS_REG_SCISSOR_1);
            appendU64(packet, 1ull << 17u); // ZTST always.
            appendU64(packet, GS_REG_TEST_1);
            appendU64(packet, 1ull << 32u); // Mask Z writes so the test framebuffer remains visible.
            appendU64(packet, GS_REG_ZBUF_1);

            constexpr uint16_t kSpritePrim = static_cast<uint16_t>(GS_PRIM_SPRITE);
            appendU64(packet, makeGifTagPrim(2u, kSpritePrim, GIF_FMT_PACKED, 3u, true, true));
            appendU64(packet, static_cast<uint64_t>(GS_REG_UV) |
                                  (static_cast<uint64_t>(GS_REG_RGBAQ) << 4u) |
                                  (static_cast<uint64_t>(GS_REG_XYZF2) << 8u));
            appendPackedUv(packet, 0u, 0u);
            appendPackedRgbaq(packet, 0x20u, 0x40u, 0x80u, 0x80u);
            appendPackedXyzf2(packet, 0u, 0u, 0u);
            appendPackedUv(packet, 0u, 0u);
            appendPackedRgbaq(packet, 0xE0u, 0x30u, 0x10u, 0x80u);
            appendPackedXyzf2(packet, 64u, 64u, 0u);

            genericGs.processGIFPacket(packet.data(), static_cast<uint32_t>(packet.size()));

            constexpr uint32_t kGifCh = 0x1000A000u;
            constexpr uint32_t kDStat = 0x1000E010u;
            constexpr uint32_t kScratchTag = 0xF0000000u;
            uint8_t *scratch = mem.getScratchpad();
            writeDmaTag(scratch, 0u, makeDmaTag(static_cast<uint16_t>(packet.size() / 16u), 7u, 0u, false));
            std::memcpy(scratch + 16u, packet.data(), packet.size());

            t.IsTrue(mem.writeIORegister(kGifCh + 0x30u, kScratchTag), "write GIF TADR scratchpad alias should succeed");
            t.IsTrue(mem.tryProcessNativeGifPackedChain(nativeGs, kScratchTag, 0x105u),
                     "packed primitive chain should use the native packed GIF path");

            t.Equals(nativeGs.nativePackedGIFPacketCount(), 1ull, "native packed GIF packet counter should increment");
            t.Equals(mem.gifCopyCount(), 1ull, "native packed GIF chain should still count as a GIF DMA copy");
            t.IsTrue((mem.readIORegister(kDStat) & (1u << 2u)) != 0u,
                     "native packed GIF chain should raise D_STAT GIF completion");
            t.Equals(mem.readIORegister(kGifCh + 0x20u), 0u, "native packed GIF chain should clear GIF QWC");
            t.Equals(mem.readIORegister(kGifCh + 0x00u) & 0x100u, 0u,
                     "native packed GIF chain should clear GIF STR");

            const uint32_t nativePixel = nativeGs.ReadVram(GS_PSM_CT32, 0u, 1u, 1u, 1u);
            const uint32_t genericPixel = genericGs.ReadVram(GS_PSM_CT32, 0u, 1u, 1u, 1u);
            t.IsTrue(genericPixel != 0u, "generic packed primitive packet should draw a test pixel");
            t.Equals(nativePixel, genericPixel, "native packed GIF chain should match generic GS packet output");
        });

        tc.Run("GIF DMA chain REF keeps CT32 image data after paletted upload", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            GS gs;
            gs.init(mem.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &mem.gs());
            std::vector<uint8_t> capturedGifPacket;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                capturedGifPacket.assign(data, data + sizeBytes);
                gs.processGIFPacket(data, sizeBytes);
            });

            constexpr uint32_t kGifCh = 0x1000A000u;
            constexpr uint32_t kChain = 0x00028000u;
            constexpr uint32_t kT4Data = 0x00029000u;
            constexpr uint32_t kCt32Data = 0x0002A000u;
            constexpr uint32_t kT4Dbp = 0u;
            constexpr uint32_t kCt32Dbp = 32u;
            constexpr uint32_t kWidth = 16u;
            constexpr uint32_t kHeight = 16u;
            constexpr uint32_t kT4Bytes = kWidth * kHeight / 2u;
            constexpr uint32_t kCt32Bytes = kWidth * kHeight * 4u;

            uint8_t *rdram = mem.getRDRAM();
            uint32_t pixel = 0u;
            for (uint32_t i = 0; i < kT4Bytes; ++i)
            {
                const uint8_t lo = static_cast<uint8_t>(pixel & 0xFu);
                const uint8_t hi = static_cast<uint8_t>((pixel + 1u) & 0xFu);
                rdram[kT4Data + i] = static_cast<uint8_t>(lo | (hi << 4));
                pixel += 2u;
                if (pixel > 0xEu)
                {
                    pixel -= 0xEu;
                }
            }

            uint32_t color = 0u;
            for (uint32_t i = 0; i < kCt32Bytes; i += 4u)
            {
                rdram[kCt32Data + i + 0u] = static_cast<uint8_t>((color >> 0) & 0xFFu);
                rdram[kCt32Data + i + 1u] = static_cast<uint8_t>((color >> 8) & 0xFFu);
                rdram[kCt32Data + i + 2u] = static_cast<uint8_t>((color >> 16) & 0xFFu);
                rdram[kCt32Data + i + 3u] = 0x80u;
                color += 0xF1u;
                if (color >= 0xFFFFFFu)
                {
                    color = 0u;
                }
            }

            uint32_t chain = kChain;
            chain = writeTextureUploadSetup(rdram, chain, kT4Dbp, GS_PSM_T4HL);
            chain = writeTextureImageRef(rdram, chain, kT4Bytes / 16u, kT4Data);
            chain = writeTextureUploadSetup(rdram, chain, kCt32Dbp, GS_PSM_CT32);
            chain = writeTextureImageRef(rdram, chain, kCt32Bytes / 16u, kCt32Data);
            writeDmaTag(rdram, chain, makeDmaTag(0u, 7u, 0u, false)); // END.

            t.IsTrue(mem.writeIORegister(kGifCh + 0x30u, kChain), "write GIF TADR should succeed");
            t.IsTrue(mem.writeIORegister(kGifCh + 0x00u, 0x104u), "write GIF CHCR STR|CHAIN should succeed");

            mem.processPendingTransfers();

            constexpr uint32_t kCt32PayloadOffset = 5u * 16u + 16u + kT4Bytes + 5u * 16u + 16u;
            t.IsTrue(capturedGifPacket.size() >= kCt32PayloadOffset + kCt32Bytes,
                     "flattened GIF chain should contain the full CT32 REF payload");
            bool ct32PayloadOk = capturedGifPacket.size() >= kCt32PayloadOffset + kCt32Bytes;
            for (uint32_t i = 0; i < kCt32Bytes && ct32PayloadOk; ++i)
            {
                if (capturedGifPacket[kCt32PayloadOffset + i] != rdram[kCt32Data + i])
                {
                    ct32PayloadOk = false;
                    break;
                }
            }
            t.IsTrue(ct32PayloadOk, "flattened GIF chain should preserve CT32 REF bytes after the T4 REF payload");

            const uint32_t row1Off = GSPSMCT32::addrPSMCT32(kCt32Dbp, 1u, 0u, 1u);
            uint32_t actualRow1X0 = 0u;
            uint32_t expectedRow1X0 = 0u;
            std::memcpy(&actualRow1X0, mem.getGSVRAM() + row1Off, sizeof(actualRow1X0));
            std::memcpy(&expectedRow1X0, rdram + kCt32Data + kWidth * 4u, sizeof(expectedRow1X0));
            t.Equals(actualRow1X0, expectedRow1X0, "CT32 row 1 must come from CT32 data, not the previous T4 REF payload");

            bool ct32Ok = true;
            for (uint32_t y = 0; y < kHeight && ct32Ok; ++y)
            {
                for (uint32_t x = 0; x < kWidth && ct32Ok; ++x)
                {
                    const uint32_t dstOff = GSPSMCT32::addrPSMCT32(kCt32Dbp, 1u, x, y);
                    const uint32_t srcOff = kCt32Data + ((y * kWidth + x) * 4u);
                    for (uint32_t c = 0; c < 4u; ++c)
                    {
                        if (mem.getGSVRAM()[dstOff + c] != rdram[srcOff + c])
                        {
                            ct32Ok = false;
                            break;
                        }
                    }
                }
            }
            t.IsTrue(ct32Ok, "all CT32 pixels should survive a preceding paletted upload in the same GIF DMA chain");
        });

        tc.Run("VIF1 DMA microprogram and FLUSH barriers precede channel completion", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "VIF1 DMA ordering fixture should initialize");

            constexpr uint32_t kVif1Channel = 0x10009000u;
            constexpr uint32_t kDStat = 0x1000E010u;
            constexpr uint32_t kSource = 0x00023800u;
            constexpr uint32_t commands[] = {
                0x03000005u, // BASE 5
                0x02000003u, // OFFSET 3
                0x04000007u, // ITOP 7
                0x14000000u, // MSCAL 0
                0x10000000u, // FLUSHE
                0x11000000u, // FLUSH
                0x13000000u, // FLUSHA
                0x17000000u, // MSCNT
            };
            static_assert(sizeof(commands) == 2u * 16u);

            std::memcpy(mem.getRDRAM() + kSource, commands, sizeof(commands));
            struct VifDispatch
            {
                uint32_t command;
                uint32_t startPc;
                uint32_t top;
                uint32_t itop;
                uint32_t tops;
                uint32_t dbf;
                uint32_t outstandingQwc;
                bool dmaCompletionPending;
            };
            std::vector<VifDispatch> dispatches;
            const auto snapshot = [&](uint32_t command, uint32_t startPc,
                                      uint32_t top, uint32_t itop)
            {
                dispatches.push_back({
                    command, startPc, top, itop,
                    mem.vif1_regs.tops,
                    (mem.vif1_regs.stat >> 7u) & 1u,
                    mem.readIORegister(kVif1Channel + 0x20u),
                    (mem.readIORegister(kDStat) & (1u << 1u)) != 0u
                });
            };
            mem.setVu1MscalCallback([&](uint32_t startPc, uint32_t top, uint32_t itop)
            {
                snapshot(0x14000000u, startPc, top, itop);
            });
            mem.setVu1MscntCallback([&](uint32_t top, uint32_t itop)
            {
                snapshot(0x17000000u, 0xFFFFFFFFu, top, itop);
            });

            t.IsTrue(mem.writeIORegister(kVif1Channel + 0x10u, kSource),
                     "VIF1 MADR should accept the source");
            t.IsTrue(mem.writeIORegister(kVif1Channel + 0x20u, 2u),
                     "VIF1 QWC should accept both command qwords");
            t.IsTrue(mem.writeIORegister(kVif1Channel, 0x100u),
                     "VIF1 CHCR should start a normal DMA");
            // The current compatibility engine drains VIF1 synchronously as
            // CHCR.STR is written; processPendingTransfers() is not the first
            // execution boundary. QWC and D_STAT are observable inside the
            // callbacks, unlike CHCR.STR whose read accessor forces idle.
            t.Equals(dispatches.size(), static_cast<size_t>(2u),
                     "CHCR start must dispatch MSCAL and MSCNT before returning");
            t.IsTrue((mem.readIORegister(kDStat) & (1u << 1u)) != 0u,
                     "the synchronous CHCR start should already report completion");

            mem.processPendingTransfers();
            t.Equals(dispatches.size(), static_cast<size_t>(2u),
                     "a redundant drain must not replay the callbacks");
            if (dispatches.size() == 2u)
            {
                const VifDispatch &first = dispatches[0];
                const VifDispatch &second = dispatches[1];
                t.Equals(first.command, 0x14000000u,
                         "the initial VU1 callback must be MSCAL");
                t.Equals(first.startPc, 0u, "MSCAL should start at code address zero");
                t.Equals(first.top, 5u, "MSCAL should see BASE as TOP");
                t.Equals(first.itop, 7u, "MSCAL should receive ITOPS");
                t.Equals(first.tops, 8u, "MSCAL should advance TOPS to BASE + OFFSET");
                t.Equals(first.dbf, 1u, "MSCAL should enter the alternate buffer");
                t.Equals(first.outstandingQwc, 2u,
                         "VIF1 QWC should remain live during MSCAL");
                t.IsFalse(first.dmaCompletionPending,
                          "DMA must not complete before the MSCAL callback returns");

                t.Equals(second.command, 0x17000000u,
                         "the resumed callback must be MSCNT after three barriers");
                t.Equals(second.top, 8u, "MSCNT should see the second buffer");
                t.Equals(second.itop, 7u, "MSCNT must preserve ITOP");
                t.Equals(second.tops, 5u, "MSCNT should rotate TOPS back to BASE");
                t.Equals(second.dbf, 0u, "MSCNT should restore the original buffer");
                t.Equals(second.outstandingQwc, 2u,
                         "VIF1 QWC should remain live during MSCNT");
                t.IsFalse(second.dmaCompletionPending,
                          "DMA must not complete before MSCNT returns");
            }

            t.Equals(mem.vif1_regs.top, 8u, "the final VIF1 TOP should be the MSCNT buffer");
            t.Equals(mem.vif1_regs.itop, 7u, "the final VIF1 ITOP should remain latched");
            t.Equals(mem.vif1_regs.tops, 5u, "TOPS should return to the first buffer");
            t.Equals(mem.vif1_regs.stat & (1u << 7u), 0u,
                     "the final DBF should select the first buffer");
            t.Equals(mem.readIORegister(kVif1Channel) & 0x100u, 0u,
                     "DMA STR should clear after the command stream drains");
            t.Equals(mem.readIORegister(kVif1Channel + 0x20u), 0u,
                     "DMA QWC should be zero after completion");
            t.IsTrue((mem.readIORegister(kDStat) & (1u << 1u)) != 0u,
                     "VIF1 DMA completion should latch the channel status");
            const std::vector<uint32_t> completed = mem.consumeCompletedDmacCauses();
            t.Equals(completed.size(), static_cast<size_t>(1u),
                     "the VIF1 DMA must report one completion");
            if (!completed.empty())
                t.Equals(completed.front(), 1u, "the completed DMAC channel must be VIF1");
            mem.processPendingTransfers();
            t.Equals(dispatches.size(), static_cast<size_t>(2u),
                     "polling again must not replay completed VU1 callbacks");
            t.IsTrue(mem.consumeCompletedDmacCauses().empty(),
                     "polling again must not requeue the completed DMAC interrupt");
        });

        tc.Run("VIF1 DMA completes when raw GIF IMAGE spans two transports", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PATH2 IMAGE split-DMA fixture should initialize");
            constexpr uint32_t kChannel = 0x10009000u;
            constexpr uint32_t kDStat = 0x1000E010u;
            constexpr uint32_t kSource = 0x00023E00u;
            uint8_t *ram = mem.getRDRAM();
            std::memset(ram + kSource, 0, 48u);

            const uint32_t direct = makeVifCmd(0x50u, 0u, 1u);
            const uint64_t imageTag = makeGifTag(1u, GIF_FMT_IMAGE, 0u, true);
            std::memcpy(ram + kSource, &direct, sizeof(direct));
            std::memcpy(ram + kSource + 4u, &imageTag, sizeof(imageTag));
            for (uint32_t i = 0u; i < 12u; ++i)
                ram[kSource + 20u + i] = static_cast<uint8_t>(0xA0u + i);
            for (uint32_t i = 0u; i < 4u; ++i)
                ram[kSource + 32u + i] = static_cast<uint8_t>(0xACu + i);
            const uint32_t mark = makeVifCmd(0x07u, 0u, 0x7788u);
            std::memcpy(ram + kSource + 36u, &mark, sizeof(mark));

            std::vector<std::vector<uint8_t>> packets;
            mem.setGifPacketCallback([&](const uint8_t *bytes, uint32_t count)
            {
                packets.emplace_back(bytes, bytes + count);
            });

            mem.writeIORegister(kChannel + 0x10u, kSource);
            mem.writeIORegister(kChannel + 0x20u, 2u);
            mem.writeIORegister(kChannel, 0x100u);
            t.Equals(packets.size(), static_cast<size_t>(1u),
                     "DIRECT must send the IMAGE tag before raw data completes");
            t.Equals(mem.readIORegister(kChannel + 0x20u), 0u,
                     "the first DMA must consume its partial IMAGE bytes");
            t.Equals(mem.readIORegister(kChannel) & 0x100u, 0u,
                     "the first DMA must clear STR with partial IMAGE data");
            t.IsTrue((mem.readIORegister(kDStat) & 2u) != 0u,
                     "DMAC must complete independently of pending raw IMAGE data");
            mem.write32(kDStat, 2u);

            mem.writeIORegister(kChannel + 0x10u, kSource + 32u);
            mem.writeIORegister(kChannel + 0x20u, 1u);
            mem.writeIORegister(kChannel, 0x100u);
            t.Equals(packets.size(), static_cast<size_t>(2u),
                     "the next DMA must assemble exactly one IMAGE quadword");
            if (packets.size() == 2u)
            {
                t.Equals(packets[1].size(), static_cast<size_t>(32u),
                         "the IMAGE continuation packet needs tag and payload");
                if (packets[1].size() == 32u)
                {
                    bool byteOrderOk = true;
                    for (uint32_t i = 0u; i < 16u; ++i)
                        byteOrderOk &= packets[1][i + 16u] == static_cast<uint8_t>(0xA0u + i);
                    t.IsTrue(byteOrderOk, "PATH2 IMAGE must preserve payload byte order");
                }
            }
            t.Equals(mem.read32(0x10003C30u), 0x7788u,
                     "a trailing VIF MARK must decode after IMAGE completion");
            t.Equals(mem.readIORegister(kChannel + 0x20u), 0u,
                     "the second DMA must clear QWC after the marker");
            t.IsTrue((mem.readIORegister(kDStat) & 2u) != 0u,
                     "the second DMA must raise its own completion");
        });

        tc.Run("VIF1 DMA DIRECT forwards payload to GIF callback and clears channel", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kSrc = 0x00024000u;
            constexpr uint32_t kQwc = 2u; // 32 bytes total transport

            uint8_t *rdram = mem.getRDRAM();
            std::memset(rdram + kSrc, 0, kQwc * 16u);

            // DIRECT 1 QW.
            const uint32_t cmd = makeVifCmd(0x50u, 0u, 1u);
            std::memcpy(rdram + kSrc, &cmd, sizeof(cmd));
            for (uint32_t i = 0; i < 16u; ++i)
            {
                rdram[kSrc + 4u + i] = static_cast<uint8_t>((0x11u + i) & 0xFFu);
            }

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                captured.emplace_back(data, data + sizeBytes);
            });

            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x10u, kSrc), "write VIF1 MADR should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x20u, kQwc), "write VIF1 QWC should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x100u), "write VIF1 CHCR STR should succeed");

            mem.processPendingTransfers();

            t.Equals(captured.size(), static_cast<size_t>(1u), "VIF1 DIRECT should emit one GIF packet");
            t.Equals(captured[0].size(), static_cast<size_t>(16u), "VIF1 DIRECT packet should be 1 QW");
            bool contentOk = true;
            for (uint32_t i = 0; i < 16u; ++i)
            {
                if (captured[0][i] != static_cast<uint8_t>((0x11u + i) & 0xFFu))
                {
                    contentOk = false;
                    break;
                }
            }
            t.IsTrue(contentOk, "VIF1 DIRECT packet bytes should match payload");
            t.IsTrue((mem.readIORegister(kVif1Ch + 0x00u) & 0x100u) == 0u, "VIF1 CHCR STR bit should be cleared after drain");
            t.Equals(mem.readIORegister(kVif1Ch + 0x20u), 0u, "VIF1 QWC should be cleared after drain");
        });

        tc.Run("VIF1 DMA chain preserves compact tag high bytes for DIRECT packets", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kTag = 0x00025000u;

            uint8_t *rdram = mem.getRDRAM();
            std::memset(rdram + kTag, 0, 32u);

            const uint64_t endTag = makeDmaTag(1u, 7u, 0u, false);
            std::memcpy(rdram + kTag, &endTag, sizeof(endTag));

            // Compact VIF1 packet helpers place the DIRECT command in the tag's upper 64 bits.
            const uint32_t directCmd = makeVifCmd(0x50u, 0u, 1u);
            std::memcpy(rdram + kTag + 12u, &directCmd, sizeof(directCmd));
            for (uint32_t i = 0; i < 16u; ++i)
            {
                rdram[kTag + 16u + i] = static_cast<uint8_t>(0x70u + i);
            }

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                captured.emplace_back(data, data + sizeBytes);
            });

            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x30u, kTag), "write VIF1 TADR should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x144u),
                     "write VIF1 CHCR STR|CHAIN|TTE should succeed");

            mem.processPendingTransfers();

            t.Equals(captured.size(), static_cast<size_t>(1u), "compact VIF1 chain should emit one GIF packet");
            t.Equals(captured[0].size(), static_cast<size_t>(16u), "compact VIF1 DIRECT packet should be 1 QW");

            bool payloadOk = true;
            for (uint32_t i = 0; i < 16u; ++i)
            {
                if (captured[0][i] != static_cast<uint8_t>(0x70u + i))
                {
                    payloadOk = false;
                    break;
                }
            }
            t.IsTrue(payloadOk, "compact VIF1 chain payload should reach the GIF callback");
            t.IsTrue((mem.readIORegister(kVif1Ch + 0x00u) & 0x100u) == 0u,
                     "compact VIF1 chain should clear the STR bit after drain");
        });

        tc.Run("VIF1 DMA chain preserves compact tag high bytes when qwc is zero", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kTag = 0x00025100u;

            uint8_t *rdram = mem.getRDRAM();
            std::memset(rdram + kTag, 0, 16u);

            const uint64_t endTag = makeDmaTag(0u, 7u, 0u, false);
            std::memcpy(rdram + kTag, &endTag, sizeof(endTag));

            const uint32_t itopCmd = makeVifCmd(0x04u, 0u, 0x44u);
            std::memcpy(rdram + kTag + 12u, &itopCmd, sizeof(itopCmd));

            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x30u, kTag), "write VIF1 TADR should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x144u),
                     "write VIF1 CHCR STR|CHAIN|TTE should succeed");

            mem.processPendingTransfers();

            t.Equals(mem.vif1_regs.itops, 0x44u,
                     "qwc-zero compact VIF1 chain should still process high-half VIFcodes");
            t.IsTrue((mem.readIORegister(kVif1Ch + 0x00u) & 0x100u) == 0u,
                     "qwc-zero compact VIF1 chain should clear the STR bit after drain");
        });

        tc.Run("VIF1 DMA chain transfers REF tag high bytes when TTE is enabled", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kTag0 = 0x00025200u;
            constexpr uint32_t kTag1 = kTag0 + 0x10u;
            constexpr uint32_t kRefPayload = 0x00025300u;

            uint8_t *rdram = mem.getRDRAM();
            writeDmaTag(rdram, kTag0, makeDmaTag(1u, 3u, kRefPayload, false)); // REF
            writeDmaTag(rdram, kTag1, makeDmaTag(0u, 7u, 0u, false));         // END

            // With CHCR.TTE set, both VIFcodes stored in every DMAtag's upper half
            // precede that tag's payload, including tags whose payload is referenced.
            const uint32_t directCmd = makeVifCmd(0x50u, 0u, 1u);
            std::memcpy(rdram + kTag0 + 12u, &directCmd, sizeof(directCmd));
            for (uint32_t i = 0; i < 16u; ++i)
            {
                rdram[kRefPayload + i] = static_cast<uint8_t>(0xA0u + i);
            }

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                captured.emplace_back(data, data + sizeBytes);
            });

            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x30u, kTag0), "write VIF1 TADR should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x144u),
                     "write VIF1 CHCR STR|CHAIN|TTE should succeed");

            mem.processPendingTransfers();

            t.Equals(captured.size(), static_cast<size_t>(1u),
                     "REF tag high-half DIRECT should emit one GIF packet");
            if (!captured.empty())
            {
                t.Equals(captured[0].size(), static_cast<size_t>(16u),
                         "REF tag high-half DIRECT packet should be 1 QW");

                bool payloadOk = true;
                for (uint32_t i = 0; i < 16u; ++i)
                {
                    if (captured[0][i] != static_cast<uint8_t>(0xA0u + i))
                    {
                        payloadOk = false;
                        break;
                    }
                }
                t.IsTrue(payloadOk, "REF payload should reach the GIF callback without VIF desynchronization");
            }
        });

        tc.Run("VIF1 DMA chain ignores tag high bytes when TTE is disabled", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kTag = 0x00025400u;

            uint8_t *rdram = mem.getRDRAM();
            writeDmaTag(rdram, kTag, makeDmaTag(0u, 7u, 0u, false)); // END
            const uint32_t itopCmd = makeVifCmd(0x04u, 0u, 0x55u);
            std::memcpy(rdram + kTag + 12u, &itopCmd, sizeof(itopCmd));

            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x30u, kTag), "write VIF1 TADR should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x104u),
                     "write VIF1 CHCR STR|CHAIN without TTE should succeed");

            mem.processPendingTransfers();

            t.Equals(mem.vif1_regs.itops, 0u,
                     "tag high-half VIFcodes must stay hidden when CHCR.TTE is clear");
        });

        tc.Run("VIF1 packet builders keep chain qwc live before terminate", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kStateAddr = 0x00027000u;
            constexpr uint32_t kBaseAddr = 0x00027100u;

            uint8_t *rdram = mem.getRDRAM();
            std::memset(rdram + kStateAddr, 0, 0x200u);

            R5900Context ctx{};
            setRegU32(ctx, 4, kStateAddr);
            setRegU32(ctx, 5, kBaseAddr);
            ps2_stubs::sceVif1PkInit(rdram, &ctx, nullptr);

            std::memset(&ctx, 0, sizeof(ctx));
            setRegU32(ctx, 4, kStateAddr);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::sceVif1PkCnt(rdram, &ctx, nullptr);

            std::memset(&ctx, 0, sizeof(ctx));
            setRegU32(ctx, 4, kStateAddr);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::sceVif1PkOpenDirectCode(rdram, &ctx, nullptr);

            std::memset(&ctx, 0, sizeof(ctx));
            setRegU32(ctx, 4, kStateAddr);
            setRegU32(ctx, 5, 4u); // reserve one qword of DIRECT payload
            ps2_stubs::sceVif1PkReserve(rdram, &ctx, nullptr);
            const uint32_t payloadAddr = ::getRegU32(&ctx, 2);
            for (uint32_t i = 0; i < 16u; ++i)
            {
                rdram[payloadAddr + i] = static_cast<uint8_t>(0x30u + i);
            }

            std::memset(&ctx, 0, sizeof(ctx));
            setRegU32(ctx, 4, kStateAddr);
            ps2_stubs::sceVif1PkCloseDirectCode(rdram, &ctx, nullptr);

            uint32_t dmaTagWord = 0u;
            std::memcpy(&dmaTagWord, rdram + kBaseAddr, sizeof(dmaTagWord));
            t.Equals(dmaTagWord & 0xFFFFu, 1u, "live packet head qwc should reflect one qword before terminate");

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                captured.emplace_back(data, data + sizeBytes);
            });

            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x30u, kBaseAddr), "write VIF1 TADR should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x144u),
                     "write VIF1 CHCR STR|CHAIN|TTE should succeed");

            mem.processPendingTransfers();

            t.Equals(captured.size(), static_cast<size_t>(1u), "live VIF1 packet should emit one GIF packet");
            t.Equals(captured[0].size(), static_cast<size_t>(16u), "live VIF1 packet should emit one qword");

            bool payloadOk = true;
            for (uint32_t i = 0; i < 16u; ++i)
            {
                if (captured[0][i] != static_cast<uint8_t>(0x30u + i))
                {
                    payloadOk = false;
                    break;
                }
            }
            t.IsTrue(payloadOk, "live VIF1 packet payload should reach the GIF callback");
        });

        tc.Run("VIF1 DMA chain latches terminal tag bits in CHCR", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kTag0 = 0x00027400u;
            constexpr uint32_t kTag1 = kTag0 + 0x20u;

            uint8_t *rdram = mem.getRDRAM();
            writeDmaTag(rdram, kTag0, makeDmaTag(1u, 1u, 0u, false)); // CNT
            std::memset(rdram + kTag0 + 0x10u, 0, 0x10u);
            writeDmaTag(rdram, kTag1, makeDmaTag(0u, 7u, 0u, false)); // END

            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x30u, kTag0), "write VIF1 TADR should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x185u), "write VIF1 CHCR chain start should succeed");

            mem.processPendingTransfers();

            const uint32_t chcr = mem.readIORegister(kVif1Ch + 0x00u);
            t.Equals(chcr & 0x100u, 0u, "VIF1 STR should clear after DMA chain drain");
            t.Equals(chcr & 0x70000000u, 0x70000000u, "VIF1 CHCR should expose the terminal END tag id");
            t.IsTrue((mem.readIORegister(0x1000E010u) & 0x2u) != 0u, "VIF1 DMA completion should raise D_STAT channel bit");
        });

        tc.Run("GIF DMA chain CALL sources payload from TADR+16", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kGifCh = 0x1000A000u;
            constexpr uint32_t kTag0 = 0x00026000u;
            constexpr uint32_t kTag1 = 0x00026100u;

            uint8_t *rdram = mem.getRDRAM();

            // CALL qwc=1 addr=kTag1
            writeDmaTag(rdram, kTag0, makeDmaTag(1u, 5u, kTag1, false));
            // END qwc=1
            writeDmaTag(rdram, kTag1, makeDmaTag(1u, 7u, 0u, false));

            for (uint32_t i = 0; i < 16u; ++i)
            {
                rdram[kTag0 + 16u + i] = static_cast<uint8_t>(0x40u + i); // CALL payload
                rdram[kTag1 + 16u + i] = static_cast<uint8_t>(0x80u + i); // END payload
            }

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                captured.emplace_back(data, data + sizeBytes);
            });

            t.IsTrue(mem.writeIORegister(kGifCh + 0x30u, kTag0), "write TADR should succeed");
            // STR + CHAIN mode (MOD=1)
            t.IsTrue(mem.writeIORegister(kGifCh + 0x00u, 0x104u), "write CHCR should succeed");

            mem.processPendingTransfers();

            t.Equals(captured.size(), static_cast<size_t>(1u), "chain CALL should emit one packet");
            t.Equals(captured[0].size(), static_cast<size_t>(32u), "CALL+END should emit two qwords");

            bool firstQwOk = true;
            bool secondQwOk = true;
            for (uint32_t i = 0; i < 16u; ++i)
            {
                if (captured[0][i] != static_cast<uint8_t>(0x40u + i))
                    firstQwOk = false;
                if (captured[0][16u + i] != static_cast<uint8_t>(0x80u + i))
                    secondQwOk = false;
            }
            t.IsTrue(firstQwOk, "CALL must transfer from TADR+16, not DMAtag ADDR");
            t.IsTrue(secondQwOk, "END payload should follow CALL payload");
        });

        tc.Run("GIF DMA chain RET transfers payload and resumes after CALL", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kGifCh = 0x1000A000u;
            constexpr uint32_t kTagCall = 0x00026200u;
            constexpr uint32_t kTagRet = 0x00026300u;
            constexpr uint32_t kTagEnd = 0x00026220u;

            uint8_t *rdram = mem.getRDRAM();

            // CALL qwc=1 -> jumps to RET tag
            writeDmaTag(rdram, kTagCall, makeDmaTag(1u, 5u, kTagRet, false));
            // RET qwc=1 -> should return to kTagEnd
            writeDmaTag(rdram, kTagRet, makeDmaTag(1u, 6u, 0u, false));
            // END qwc=1 after CALL payload
            writeDmaTag(rdram, kTagEnd, makeDmaTag(1u, 7u, 0u, false));

            for (uint32_t i = 0; i < 16u; ++i)
            {
                rdram[kTagCall + 16u + i] = static_cast<uint8_t>(0x11u + i); // CALL payload
                rdram[kTagRet + 16u + i] = static_cast<uint8_t>(0x22u + i);  // RET payload
                rdram[kTagEnd + 16u + i] = static_cast<uint8_t>(0x33u + i);  // END payload
            }

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                captured.emplace_back(data, data + sizeBytes);
            });

            t.IsTrue(mem.writeIORegister(kGifCh + 0x30u, kTagCall), "write TADR should succeed");
            t.IsTrue(mem.writeIORegister(kGifCh + 0x00u, 0x104u), "write CHCR should succeed");

            mem.processPendingTransfers();

            t.Equals(captured.size(), static_cast<size_t>(1u), "CALL/RET chain should emit one packet");
            t.Equals(captured[0].size(), static_cast<size_t>(48u), "CALL+RET+END should emit three qwords");

            bool q0 = true;
            bool q1 = true;
            bool q2 = true;
            for (uint32_t i = 0; i < 16u; ++i)
            {
                if (captured[0][i] != static_cast<uint8_t>(0x11u + i))
                    q0 = false;
                if (captured[0][16u + i] != static_cast<uint8_t>(0x22u + i))
                    q1 = false;
                if (captured[0][32u + i] != static_cast<uint8_t>(0x33u + i))
                    q2 = false;
            }
            t.IsTrue(q0, "CALL payload should be first");
            t.IsTrue(q1, "RET must still transfer its own payload");
            t.IsTrue(q2, "RET must resume after CALL payload and continue chain");
        });

        tc.Run("GIF DMA chain IRQ stops only when TIE is set", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kGifCh = 0x1000A000u;
            constexpr uint32_t kTag0 = 0x00026400u;
            constexpr uint32_t kTag1 = 0x00026410u;
            constexpr uint32_t kRefData = 0x00026500u;

            auto runChain = [&](uint32_t chcrValue, std::vector<uint8_t> &packetOut) -> bool
            {
                uint8_t *rdram = mem.getRDRAM();
                writeDmaTag(rdram, kTag0, makeDmaTag(1u, 3u, kRefData, true)); // REF + IRQ
                writeDmaTag(rdram, kTag1, makeDmaTag(1u, 7u, 0u, false));       // END
                for (uint32_t i = 0; i < 16u; ++i)
                {
                    rdram[kRefData + i] = static_cast<uint8_t>(0x55u + i);
                    rdram[kTag1 + 16u + i] = static_cast<uint8_t>(0x77u + i);
                }

                std::vector<std::vector<uint8_t>> captured;
                mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
                {
                    captured.emplace_back(data, data + sizeBytes);
                });

                if (!mem.writeIORegister(kGifCh + 0x30u, kTag0))
                    return false;
                if (!mem.writeIORegister(kGifCh + 0x00u, chcrValue))
                    return false;
                mem.processPendingTransfers();
                if (captured.empty())
                    return false;
                packetOut = captured[0];
                return true;
            };

            std::vector<uint8_t> packetNoTie;
            t.IsTrue(runChain(0x104u, packetNoTie), "chain run without TIE should succeed");
            t.Equals(packetNoTie.size(), static_cast<size_t>(32u), "IRQ tag should not stop chain when TIE is clear");

            std::vector<uint8_t> packetTie;
            // STR + CHAIN + TIE(bit7)
            t.IsTrue(runChain(0x184u, packetTie), "chain run with TIE should succeed");
            t.Equals(packetTie.size(), static_cast<size_t>(16u), "IRQ tag should stop chain when TIE is set");
        });

        tc.Run("DMAC D_STAT toggles masks and clears channel status on write-one", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kDStat = 0x1000E010u;
            constexpr uint32_t kGifMaskBit = (1u << 18); // channel 2 mask
            constexpr uint32_t kGifStatusBit = (1u << 2); // channel 2 status
            constexpr uint32_t kSummaryBit = (1u << 31);

            t.IsTrue(mem.writeIORegister(kDStat, kGifMaskBit), "D_STAT mask toggle write should succeed");
            t.IsTrue((mem.readIORegister(kDStat) & kGifMaskBit) != 0u, "first mask write should enable GIF mask bit");
            t.IsTrue(mem.writeIORegister(kDStat, kGifMaskBit), "D_STAT mask toggle write should succeed");
            t.IsTrue((mem.readIORegister(kDStat) & kGifMaskBit) == 0u, "second mask write should disable GIF mask bit");

            t.IsTrue(mem.writeIORegister(kDStat, kGifMaskBit), "re-enable GIF mask for summary test");

            constexpr uint32_t kGifCh = 0x1000A000u;
            constexpr uint32_t kSrc = 0x00027000u;
            uint8_t *rdram = mem.getRDRAM();
            for (uint32_t i = 0; i < 16u; ++i)
            {
                rdram[kSrc + i] = static_cast<uint8_t>(0x90u + i);
            }

            t.IsTrue(mem.writeIORegister(kGifCh + 0x10u, kSrc), "write MADR should succeed");
            t.IsTrue(mem.writeIORegister(kGifCh + 0x20u, 1u), "write QWC should succeed");
            t.IsTrue(mem.writeIORegister(kGifCh + 0x00u, 0x100u), "write CHCR STR should succeed");

            t.IsTrue((mem.readIORegister(kDStat) & kGifStatusBit) == 0u, "D_STAT status should not set before transfer drain");

            mem.processPendingTransfers();

            const uint32_t dstatAfter = mem.readIORegister(kDStat);
            t.IsTrue((dstatAfter & kGifStatusBit) != 0u, "GIF transfer completion should set D_STAT channel status bit");
            t.IsTrue((dstatAfter & kSummaryBit) != 0u, "status&mask should raise D_STAT summary bit");

            t.IsTrue(mem.writeIORegister(kDStat, kGifStatusBit), "D_STAT status clear write should succeed");
            const uint32_t dstatCleared = mem.readIORegister(kDStat);
            t.IsTrue((dstatCleared & kGifStatusBit) == 0u, "write-one should clear GIF channel status bit");
            t.IsTrue((dstatCleared & kSummaryBit) == 0u, "summary bit should clear after status clear");
        });

        tc.Run("DMAC D_CTRL DMAE gates GIF DMA start", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kDctrl = 0x1000E000u;
            constexpr uint32_t kGifCh = 0x1000A000u;
            constexpr uint32_t kSrc = 0x00027800u;

            uint8_t *rdram = mem.getRDRAM();
            for (uint32_t i = 0; i < 16u; ++i)
            {
                rdram[kSrc + i] = static_cast<uint8_t>(0xE0u + i);
            }

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                captured.emplace_back(data, data + sizeBytes);
            });

            t.IsTrue(mem.writeIORegister(kDctrl, 0u), "clearing D_CTRL.DMAE should succeed");
            t.IsTrue(mem.writeIORegister(kGifCh + 0x10u, kSrc), "write MADR should succeed");
            t.IsTrue(mem.writeIORegister(kGifCh + 0x20u, 1u), "write QWC should succeed");
            t.IsTrue(mem.writeIORegister(kGifCh + 0x00u, 0x100u), "write CHCR STR should succeed");
            mem.processPendingTransfers();

            t.Equals(captured.size(), static_cast<size_t>(0u), "DMAE=0 should prevent GIF DMA transfer");
            t.Equals(mem.dmaStartCount(), 0ull, "DMAE=0 should not increment dmaStartCount");

            t.IsTrue(mem.writeIORegister(kDctrl, 1u), "setting D_CTRL.DMAE should succeed");
            t.IsTrue(mem.writeIORegister(kGifCh + 0x00u, 0x100u), "restarting GIF DMA should succeed");
            mem.processPendingTransfers();

            t.Equals(captured.size(), static_cast<size_t>(1u), "DMAE=1 should allow GIF DMA transfer");
            if (!captured.empty())
            {
                t.Equals(captured[0].size(), static_cast<size_t>(16u), "GIF DMA transfer should emit one qword");
            }
        });

        tc.Run("DMAC SPR_FROM copies scratchpad to RDRAM and completes channel 8", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kChannel = 0x1000D000u;
            constexpr uint32_t kMadr = 0x00028000u;
            constexpr uint32_t kSadr = 0x00000120u;
            constexpr uint32_t kQwc = 2u;
            constexpr uint32_t kBytes = kQwc * 16u;

            for (uint32_t i = 0; i < kBytes; ++i)
                mem.getScratchpad()[kSadr + i] = static_cast<uint8_t>(0x30u + i);

            t.IsTrue(mem.writeIORegister(kChannel + 0x10u, kMadr), "write SPR_FROM MADR should succeed");
            t.IsTrue(mem.writeIORegister(kChannel + 0x20u, kQwc), "write SPR_FROM QWC should succeed");
            t.IsTrue(mem.writeIORegister(kChannel + 0x80u, kSadr), "write SPR_FROM SADR should succeed");
            t.IsTrue(mem.writeIORegister(kChannel + 0x00u, 0x100u), "start SPR_FROM should succeed");

            bool copied = true;
            for (uint32_t i = 0; i < kBytes; ++i)
                copied = copied && mem.getRDRAM()[kMadr + i] == static_cast<uint8_t>(0x30u + i);
            t.IsTrue(copied, "SPR_FROM should copy every qword from scratchpad to RDRAM");
            t.IsTrue((mem.readIORegister(kChannel + 0x00u) & 0x100u) == 0u, "SPR_FROM completion should clear CHCR.STR");
            t.Equals(mem.readIORegister(kChannel + 0x20u), 0u, "SPR_FROM completion should consume QWC");
            t.Equals(mem.readIORegister(kChannel + 0x10u), kMadr + kBytes, "SPR_FROM should advance MADR");
            t.Equals(mem.readIORegister(kChannel + 0x80u), (kSadr + kBytes) & 0x3FFFu, "SPR_FROM should advance SADR");
            t.IsTrue((mem.readIORegister(0x1000E010u) & (1u << 8u)) != 0u, "SPR_FROM should raise D_STAT channel 8");

            const std::vector<uint32_t> causes = mem.consumeCompletedDmacCauses();
            t.Equals(causes.size(), static_cast<size_t>(1u), "SPR_FROM should queue one DMAC completion");
            if (!causes.empty())
                t.Equals(causes[0], 8u, "SPR_FROM completion should use DMAC cause 8");
        });

        tc.Run("DMAC SPR_TO copies RDRAM to scratchpad and completes channel 9", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kChannel = 0x1000D400u;
            constexpr uint32_t kMadr = 0x00028400u;
            constexpr uint32_t kSadr = 0x00000240u;
            constexpr uint32_t kQwc = 2u;
            constexpr uint32_t kBytes = kQwc * 16u;

            for (uint32_t i = 0; i < kBytes; ++i)
                mem.getRDRAM()[kMadr + i] = static_cast<uint8_t>(0x70u + i);

            t.IsTrue(mem.writeIORegister(kChannel + 0x10u, kMadr), "write SPR_TO MADR should succeed");
            t.IsTrue(mem.writeIORegister(kChannel + 0x20u, kQwc), "write SPR_TO QWC should succeed");
            t.IsTrue(mem.writeIORegister(kChannel + 0x80u, kSadr), "write SPR_TO SADR should succeed");
            t.IsTrue(mem.writeIORegister(kChannel + 0x00u, 0x100u), "start SPR_TO should succeed");

            bool copied = true;
            for (uint32_t i = 0; i < kBytes; ++i)
                copied = copied && mem.getScratchpad()[kSadr + i] == static_cast<uint8_t>(0x70u + i);
            t.IsTrue(copied, "SPR_TO should copy every qword from RDRAM to scratchpad");
            t.IsTrue((mem.readIORegister(kChannel + 0x00u) & 0x100u) == 0u, "SPR_TO completion should clear CHCR.STR");
            t.Equals(mem.readIORegister(kChannel + 0x20u), 0u, "SPR_TO completion should consume QWC");
            t.IsTrue((mem.readIORegister(0x1000E010u) & (1u << 9u)) != 0u, "SPR_TO should raise D_STAT channel 9");

            const std::vector<uint32_t> causes = mem.consumeCompletedDmacCauses();
            t.Equals(causes.size(), static_cast<size_t>(1u), "SPR_TO should queue one DMAC completion");
            if (!causes.empty())
                t.Equals(causes[0], 9u, "SPR_TO completion should use DMAC cause 9");
        });

        tc.Run("sceDmaReset re-enables DMAC DMAE", [](TestCase &t)
        {
            PS2Runtime runtime;
            t.IsTrue(runtime.memory().initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kDctrl = 0x1000E000u;
            constexpr uint32_t kDpcr = 0x1000E020u;
            constexpr uint32_t kDsqwc = 0x1000E030u;
            constexpr uint32_t kDrbor = 0x1000E050u;
            constexpr uint32_t kDrbsr = 0x1000E040u;
            constexpr uint32_t kDstadr = 0x1000E060u;

            PS2Memory &mem = runtime.memory();
            t.IsTrue(mem.writeIORegister(kDctrl, 0u), "clearing D_CTRL should succeed");
            t.IsTrue(mem.writeIORegister(kDpcr, 0x12345678u), "writing D_PCR should succeed");
            t.IsTrue(mem.writeIORegister(kDsqwc, 0x11220044u), "writing D_SQWC should succeed");
            t.IsTrue(mem.writeIORegister(kDrbor, 0x2000u), "writing D_RBOR should succeed");
            t.IsTrue(mem.writeIORegister(kDrbsr, 0x3FFFu), "writing D_RBSR should succeed");
            t.IsTrue(mem.writeIORegister(kDstadr, 0x4567u), "writing D_STADR should succeed");

            R5900Context ctx{};
            ps2_stubs::sceDmaReset(mem.getRDRAM(), &ctx, &runtime);

            t.Equals(static_cast<int32_t>(::getRegU32(&ctx, 2)), 0, "sceDmaReset should return 0");
            t.Equals(mem.readIORegister(kDctrl), 1u, "sceDmaReset should leave D_CTRL DMAE enabled");
            t.Equals(mem.readIORegister(kDpcr), 0u, "sceDmaReset should clear D_PCR");
            t.Equals(mem.readIORegister(kDsqwc), 0u, "sceDmaReset should clear D_SQWC");
            t.Equals(mem.readIORegister(kDrbor), 0u, "sceDmaReset should clear D_RBOR");
            t.Equals(mem.readIORegister(kDrbsr), 0u, "sceDmaReset should clear D_RBSR");
            t.Equals(mem.readIORegister(kDstadr), 0u, "sceDmaReset should clear D_STADR");
        });

        tc.Run("sceDmaSend preserves guest-configured VIF1 TTE", [](TestCase &t)
        {
            PS2Runtime runtime;
            t.IsTrue(runtime.memory().initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kTag = 0x00028500u;

            PS2Memory &mem = runtime.memory();
            uint8_t *rdram = mem.getRDRAM();
            writeDmaTag(rdram, kTag, makeDmaTag(0u, 7u, 0u, false)); // END
            const uint32_t itopCmd = makeVifCmd(0x04u, 0u, 0x66u);
            std::memcpy(rdram + kTag + 12u, &itopCmd, sizeof(itopCmd));

            // Fatal Frame follows this exact sequence: get channel, set CHCR.TTE,
            // then submit the chain through sceDmaSend.
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x40u),
                     "guest should be able to configure VIF1 CHCR.TTE before submission");

            R5900Context ctx{};
            setRegU32(ctx, 4, 1u); // sceDmaGetChan(1) / VIF1
            setRegU32(ctx, 5, kTag);
            ps2_stubs::sceDmaSend(rdram, &ctx, &runtime);

            t.Equals(static_cast<int32_t>(::getRegU32(&ctx, 2)), 0,
                     "sceDmaSend should accept the VIF1 chain");
            t.IsTrue((mem.readIORegister(kVif1Ch + 0x00u) & 0x40u) != 0u,
                     "sceDmaSend must preserve guest-configured CHCR.TTE");
            t.Equals(mem.vif1_regs.itops, 0x66u,
                     "preserved TTE should deliver the tag high-half VIFcode");
        });

        tc.Run("VIF1 DMA DIRECT image packet reaches GS through arbiter", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            GS gs;
            gs.init(mem.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &mem.gs());
            GifArbiter arbiter([&](const uint8_t *data, uint32_t sizeBytes)
            {
                gs.processGIFPacket(data, sizeBytes);
            });
            mem.setGifArbiter(&arbiter);

            const uint64_t bitblt =
                (static_cast<uint64_t>(0u) << 0) |
                (static_cast<uint64_t>(1u) << 16) |
                (static_cast<uint64_t>(0u) << 24) |
                (static_cast<uint64_t>(0u) << 32) |
                (static_cast<uint64_t>(1u) << 48) |
                (static_cast<uint64_t>(0u) << 56);
            gs.writeRegister(GS_REG_BITBLTBUF, bitblt);
            gs.writeRegister(GS_REG_TRXPOS, 0ull);
            gs.writeRegister(GS_REG_TRXREG, (4ull << 0) | (1ull << 32));
            gs.writeRegister(GS_REG_TRXDIR, 0ull);

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kSrc = 0x00027C00u;
            constexpr uint32_t kQwc = 3u;

            uint8_t *rdram = mem.getRDRAM();
            std::memset(rdram + kSrc, 0, kQwc * 16u);

            const uint32_t directCmd = makeVifCmd(0x50u, 0u, 2u); // DIRECT 2 QW payload.
            std::memcpy(rdram + kSrc, &directCmd, sizeof(directCmd));

            uint8_t *gifPayload = rdram + kSrc + 4u;
            const uint64_t gifTag = makeGifTag(1u, GIF_FMT_IMAGE, 0u, true);
            std::memcpy(gifPayload + 0u, &gifTag, sizeof(gifTag));
            const uint64_t tagHi = 0u;
            std::memcpy(gifPayload + 8u, &tagHi, sizeof(tagHi));
            for (uint32_t i = 0; i < 16u; ++i)
            {
                gifPayload[16u + i] = static_cast<uint8_t>(0x70u + i);
            }

            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x10u, kSrc), "write VIF1 MADR should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x20u, kQwc), "write VIF1 QWC should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x100u), "write VIF1 CHCR STR should succeed");

            mem.processPendingTransfers();

            const uint8_t *vramOut = mem.getGSVRAM();
            bool imageOk = true;
            for (uint32_t x = 0; x < 4u && imageOk; ++x)
            {
                const uint32_t off = GSPSMCT32::addrPSMCT32(0u, 1u, x, 0u);
                for (uint32_t c = 0; c < 4u; ++c)
                {
                    if (vramOut[off + c] != static_cast<uint8_t>(0x70u + x * 4u + c))
                    {
                        imageOk = false;
                        break;
                    }
                }
            }
            t.IsTrue(imageOk, "VIF1 DIRECT image should update GS VRAM through GIF path2");
        });

        tc.Run("VIF1 DIRECT image tag can continue with raw image qwords", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            GS gs;
            gs.init(mem.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &mem.gs());
            GifArbiter arbiter([&](const uint8_t *data, uint32_t sizeBytes)
            {
                gs.processGIFPacket(data, sizeBytes);
            });
            mem.setGifArbiter(&arbiter);

            const uint64_t bitblt =
                (static_cast<uint64_t>(0u) << 0) |
                (static_cast<uint64_t>(1u) << 16) |
                (static_cast<uint64_t>(0u) << 24) |
                (static_cast<uint64_t>(0u) << 32) |
                (static_cast<uint64_t>(1u) << 48) |
                (static_cast<uint64_t>(0u) << 56);
            gs.writeRegister(GS_REG_BITBLTBUF, bitblt);
            gs.writeRegister(GS_REG_TRXPOS, 0ull);
            gs.writeRegister(GS_REG_TRXREG, (4ull << 0) | (1ull << 32));
            gs.writeRegister(GS_REG_TRXDIR, 0ull);

            std::vector<uint8_t> packet;
            appendU32(packet, makeVifCmd(0x50u, 0u, 1u)); // DIRECT 1 QW payload: GIF IMAGE tag only.
            appendU64(packet, makeGifTag(1u, GIF_FMT_IMAGE, 0u, true));
            appendU64(packet, 0ull);
            for (uint32_t i = 0; i < 16u; ++i)
            {
                packet.push_back(static_cast<uint8_t>(0xA0u + i));
            }

            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            const uint8_t *vramOut = mem.getGSVRAM();
            bool imageOk = true;
            for (uint32_t x = 0; x < 4u && imageOk; ++x)
            {
                const uint32_t off = GSPSMCT32::addrPSMCT32(0u, 1u, x, 0u);
                for (uint32_t c = 0; c < 4u; ++c)
                {
                    if (vramOut[off + c] != static_cast<uint8_t>(0xA0u + x * 4u + c))
                    {
                        imageOk = false;
                        break;
                    }
                }
            }
            t.IsTrue(imageOk, "raw qwords after a DIRECT image tag should continue the PATH2 image upload");
        });

        tc.Run("VIF1 DIRECT finds an image continuation after packed setup", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            GS gs;
            gs.init(mem.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &mem.gs());
            GifArbiter arbiter([&](const uint8_t *data, uint32_t sizeBytes)
            {
                gs.processGIFPacket(data, sizeBytes);
            });
            mem.setGifArbiter(&arbiter);

            const uint64_t bitblt =
                (static_cast<uint64_t>(1u) << 16) |
                (static_cast<uint64_t>(1u) << 48);
            gs.writeRegister(GS_REG_BITBLTBUF, bitblt);
            gs.writeRegister(GS_REG_TRXPOS, 0ull);
            gs.writeRegister(GS_REG_TRXREG, (4ull << 0) | (1ull << 32));
            gs.writeRegister(GS_REG_TRXDIR, 0ull);

            std::vector<uint8_t> packet;
            appendU32(packet, makeVifCmd(0x50u, 0u, 3u)); // PACKED tag + A+D + IMAGE tag.
            appendU64(packet, makeGifTag(1u, GIF_FMT_PACKED, 1u, false));
            appendU64(packet, 0x0Eull);
            appendU64(packet, 0x8000008000ull); // TEXA, harmless setup preceding the IMAGE tag.
            appendU64(packet, GS_REG_TEXA);
            appendU64(packet, makeGifTag(1u, GIF_FMT_IMAGE, 0u, true));
            appendU64(packet, 0ull);
            for (uint32_t i = 0; i < 16u; ++i)
                packet.push_back(static_cast<uint8_t>(0xC0u + i));

            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            const uint8_t *vramOut = mem.getGSVRAM();
            bool imageOk = true;
            for (uint32_t x = 0; x < 4u && imageOk; ++x)
            {
                const uint32_t off = GSPSMCT32::addrPSMCT32(0u, 1u, x, 0u);
                for (uint32_t c = 0; c < 4u; ++c)
                {
                    if (vramOut[off + c] != static_cast<uint8_t>(0xC0u + x * 4u + c))
                    {
                        imageOk = false;
                        break;
                    }
                }
            }
            t.IsTrue(imageOk, "raw image continuation after packed setup should not be decoded as VIF/GIF registers");
        });

        tc.Run("unaligned accesses throw", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            bool threwRead32 = false;
            bool threwWrite64 = false;
            try
            {
                (void)mem.read32(0x00000002u);
            }
            catch (const std::exception &)
            {
                threwRead32 = true;
            }

            try
            {
                mem.write64(0x00000004u + 2u, 0x1122334455667788ull);
            }
            catch (const std::exception &)
            {
                threwWrite64 = true;
            }

            t.IsTrue(threwRead32, "unaligned read32 should throw");
            t.IsTrue(threwWrite64, "unaligned write64 should throw");
        });
    });
}
