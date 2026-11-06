#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

class PS2Memory;
struct VU1State;

// One optional kzvu session bound to the thread that runs VIF1.
class PS2Vu1Kzvu
{
public:
    PS2Vu1Kzvu(PS2Memory &memory, bool enabled);
    ~PS2Vu1Kzvu();
    PS2Vu1Kzvu(const PS2Vu1Kzvu &) = delete;
    PS2Vu1Kzvu &operator=(const PS2Vu1Kzvu &) = delete;

    static bool tryExecute(PS2Memory &memory, VU1State &state, std::optional<uint32_t> startPc,
                           uint32_t top, uint32_t itop, uint32_t maxCycles);

private:
    static void submitPacket(void *user, const uint8_t *data, uint32_t size, uint32_t startQw);
    void execute(VU1State &state, std::optional<uint32_t> startPc,
                 uint32_t top, uint32_t itop, uint32_t maxCycles);

    PS2Memory &m_memory;
    std::unique_lock<std::mutex> m_libraryLock;
    std::optional<uint64_t> m_codeGeneration;
    unsigned m_hostMxcsr = 0;
};
