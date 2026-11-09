#include "ps2_vu1_kzvu.h"

#include "kzvu.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"

#include <cstring>
#include <immintrin.h>
#include <stdexcept>
#include <string>

namespace
{
    std::mutex libraryMutex;
    thread_local PS2Vu1Kzvu *activeSession = nullptr;

    struct ScopedMxcsr
    {
        unsigned saved = _mm_getcsr();
        ~ScopedMxcsr() { _mm_setcsr(saved); }
    };
}

PS2Vu1Kzvu::PS2Vu1Kzvu(PS2Memory &memory, bool enabled) : m_memory(memory)
{
    if (!enabled)
        return;

    m_libraryLock = std::unique_lock(libraryMutex, std::try_to_lock);
    if (!m_libraryLock.owns_lock() || kzvuIsInit())
        throw std::runtime_error("The kzvu VU1 backend requires exclusive ownership of kzvu");

    ScopedMxcsr fp;
    KzvuConfig config;
    config.useJit = false;
    config.iBitHack = false;
    config.flagHack = false;
    config.xgkick = submitPacket;
    config.xgkickUser = this;
    std::string error;
    if (!kzvuInit(config, &error))
        throw std::runtime_error("Could not initialize the kzvu VU1 backend: " + error);
    activeSession = this;
}

PS2Vu1Kzvu::~PS2Vu1Kzvu()
{
    if (activeSession != this)
        return;
    activeSession = nullptr;
    ScopedMxcsr fp;
    kzvuShutdown();
}

bool PS2Vu1Kzvu::ownsSession(const PS2Memory &memory) noexcept
{
    return activeSession && &activeSession->m_memory == &memory;
}

bool PS2Vu1Kzvu::tryExecute(PS2Memory &memory, VU1State &state, std::optional<uint32_t> startPc,
                           uint32_t top, uint32_t itop, uint32_t maxCycles)
{
    if (!ownsSession(memory))
        return false;
    activeSession->execute(state, startPc, top, itop, maxCycles);
    return true;
}

void PS2Vu1Kzvu::submitPacket(void *user, const uint8_t *data, uint32_t size, uint32_t)
{
    auto &session = *static_cast<PS2Vu1Kzvu *>(user);
    ScopedMxcsr fp;
    _mm_setcsr(session.m_hostMxcsr);
    session.m_memory.submitGifPacket(GifPathId::Path1, data, size);
}

void PS2Vu1Kzvu::execute(VU1State &state, std::optional<uint32_t> startPc,
                        uint32_t top, uint32_t itop, uint32_t maxCycles)
{
    state.interruptRaised = false;
    state.budgetExhausted = false;
    state.stoppedByD = false;
    state.stoppedByT = false;
    ScopedMxcsr fp;
    m_hostMxcsr = fp.saved;

    const bool continuing = kzvuRunning();
    const uint64_t generation = m_memory.getVU1CodeGeneration();
    if (continuing && startPc.has_value())
        throw std::runtime_error("Cannot issue MSCAL while the kzvu VU1 is still running");
    if (continuing && m_codeGeneration != generation)
        throw std::runtime_error("VU1 code memory changed while the kzvu backend was running");

    if (!continuing)
    {
        if (m_codeGeneration != generation)
        {
            kzvuWriteMicro(0, m_memory.getVU1Code(), PS2_VU1_CODE_SIZE);
            m_codeGeneration = generation;
        }

        if (startPc.has_value())
            state.cycles = 0u;
        auto *const data = m_memory.getVU1Data();
        std::memcpy(kzvuDataMem(), data, PS2_VU1_DATA_SIZE);
        kzvuSetTop(top, itop);
        kzvuSetFBRST((state.dBitEnabled ? 1u << 10 : 0u) |
                     (state.tBitEnabled ? 1u << 11 : 0u));
    }

    // MSCNT and budget continuation both resume at the saved TPC.
    const uint32_t entryPc = continuing ? 0xffffffffu : startPc.value_or(0xffffffffu);
    state.cycles += kzvuExecute(entryPc, maxCycles);
    state.pc = kzvuGetVI(kKzvuTPC) * 8u;
    state.budgetExhausted = kzvuRunning();

    // Keep VU data stores visible at every slice boundary. Do not reload this
    // buffer on a continuation: KZVU still owns the in-flight program state.
    auto *const data = m_memory.getVU1Data();
    std::memcpy(data, kzvuDataMem(), PS2_VU1_DATA_SIZE);
    if (state.budgetExhausted)
        return;

    const uint32_t stat = kzvuVpuStat();
    state.stoppedByD = (stat & 0x200u) != 0u;
    state.stoppedByT = (stat & 0x400u) != 0u;
    state.interruptRaised = kzvuTakeInterrupt();
}
