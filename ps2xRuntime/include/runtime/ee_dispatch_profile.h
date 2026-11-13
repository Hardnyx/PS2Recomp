#pragma once

// Optional diagnostics for recompiled EE call paths. Normal builds compile out
// the hot-path hooks; an instrumented build still requires PS2X_EE_PROFILE=1.
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>

#if defined(PS2X_ENABLE_EE_DISPATCH_PROFILE)
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <utility>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif
#endif

namespace ps2x::ee_profile
{
enum class CallKind : uint8_t
{
    Outer,
    Nested,
};

struct CallKey
{
    CallKind kind = CallKind::Outer;
    uint32_t caller = 0u;
    uint32_t target = 0u;
    auto operator<=>(const CallKey &) const = default;
};

struct CallTotals
{
    uint64_t samples = 0u;
    uint64_t cpuSamples = 0u;
    uint64_t cpuUnits = 0u;
    uint64_t wallNanoseconds = 0u;
};

class Accumulator
{
public:
    explicit Accumulator(size_t maxKeys = 256u) : m_maxKeys(maxKeys) {}

    void add(CallKey key, uint64_t cpuUnits, uint64_t wallNanoseconds, bool hasCpu)
    {
        auto it = m_rows.find(key);
        if (it == m_rows.end())
        {
            if (m_rows.size() >= m_maxKeys)
            {
                ++m_dropped;
                return;
            }
            it = m_rows.emplace(key, CallTotals{}).first;
        }
        auto &row = it->second;
        ++row.samples;
        row.wallNanoseconds += wallNanoseconds;
        if (hasCpu)
        {
            ++row.cpuSamples;
            row.cpuUnits += cpuUnits;
        }
    }

    [[nodiscard]] const std::map<CallKey, CallTotals> &rows() const noexcept { return m_rows; }
    [[nodiscard]] uint64_t dropped() const noexcept { return m_dropped; }
    void clear()
    {
        m_rows.clear();
        m_dropped = 0u;
    }

private:
    size_t m_maxKeys;
    std::map<CallKey, CallTotals> m_rows;
    uint64_t m_dropped = 0u;
};

#if defined(PS2X_ENABLE_EE_DISPATCH_PROFILE)
namespace detail
{
inline bool enabled() noexcept
{
    static const bool active = [] {
        const char *value = std::getenv("PS2X_EE_PROFILE");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return active;
}

inline bool readSampleCpu(uint64_t &value) noexcept
{
#if defined(_WIN32)
    ULONG64 cycles = 0u;
    if (!QueryThreadCycleTime(GetCurrentThread(), &cycles))
        return false;
    value = static_cast<uint64_t>(cycles);
    return true;
#elif defined(CLOCK_THREAD_CPUTIME_ID)
    timespec now{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0)
        return false;
    value = uint64_t(now.tv_sec) * 1000000000ull + uint64_t(now.tv_nsec);
    return true;
#else
    (void)value;
    return false;
#endif
}

inline bool readThreadCpuNs(uint64_t &value) noexcept
{
#if defined(_WIN32)
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
        return false;
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    value = (k.QuadPart + u.QuadPart) * 100u;
    return true;
#else
    return readSampleCpu(value);
#endif
}

struct State
{
    Accumulator measurements;
    std::array<uint64_t, 2> attempts{};
    uint32_t randomState = 0x6d2b79f5u;
    uint64_t firstField = 0u;
    uint64_t cpuAtFirstFieldNs = 0u;
    bool hasCpuStart = false;
};

inline State &state()
{
    static thread_local State instance;
    return instance;
}

inline void resetWindow(State &s, uint64_t field)
{
    s.firstField = field;
    s.measurements.clear();
    s.attempts = {};
    s.hasCpuStart = readThreadCpuNs(s.cpuAtFirstFieldNs);
}

inline void emit(State &s, uint64_t field)
{
    if (s.firstField == 0u || field <= s.firstField)
        return;
    uint64_t cpuNow = 0u;
    const bool validCpu = s.hasCpuStart && readThreadCpuNs(cpuNow) && cpuNow >= s.cpuAtFirstFieldNs;
    const double cpuMsPerField = validCpu
        ? double(cpuNow - s.cpuAtFirstFieldNs) / (1000000.0 * double(field - s.firstField))
        : -1.0;
    std::cerr << "[ps2x:ee-profile] fields=" << (field - s.firstField)
              << " end=" << field
              << " ee_thread_cpu_ms_per_field=" << cpuMsPerField
              << " outer_calls=" << s.attempts[0]
              << " nested_calls=" << s.attempts[1]
              << " dropped_keys=" << s.measurements.dropped() << '\n';

    std::vector<std::pair<CallKey, CallTotals>> ranked(
        s.measurements.rows().begin(), s.measurements.rows().end());
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
        if (a.second.cpuUnits != b.second.cpuUnits)
            return a.second.cpuUnits > b.second.cpuUnits;
        return a.second.wallNanoseconds > b.second.wallNanoseconds;
    });
    std::array<unsigned, 2> printed{};
    for (const auto &[key, totals] : ranked)
    {
        const size_t index = static_cast<size_t>(key.kind);
        if (printed[index]++ >= 8u)
            continue;
        std::cerr << "[ps2x:ee-profile] kind=" << (index == 0u ? "outer" : "nested")
                  << " caller=0x" << std::hex << key.caller
                  << " target=0x" << key.target << std::dec
                  << " samples=" << totals.samples
                  << " cpu_samples=" << totals.cpuSamples
                  << " cpu_units=" << totals.cpuUnits
                  << " wall_ns=" << totals.wallNanoseconds << '\n';
    }
    // Inclusive nested and outer samples overlap: never add their totals.
}
} // namespace detail

class ScopedCall
{
public:
    ScopedCall(CallKind kind, uint32_t caller, uint32_t target) noexcept
        : m_key{kind, caller, target}
    {
        if (!detail::enabled())
            return;
        auto &s = detail::state();
        ++s.attempts[static_cast<size_t>(kind)];
        uint32_t &rng = s.randomState;
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        if ((rng & 255u) != 0u)
            return;
        m_hasCpu = detail::readSampleCpu(m_startCpu);
        m_startWall = std::chrono::steady_clock::now();
        m_sampled = true;
    }

    ScopedCall(const ScopedCall &) = delete;
    ScopedCall &operator=(const ScopedCall &) = delete;

    ~ScopedCall() noexcept
    {
        if (!m_sampled)
            return;
        uint64_t cpuEnd = 0u;
        const bool validCpu = m_hasCpu && detail::readSampleCpu(cpuEnd) && cpuEnd >= m_startCpu;
        const auto wall = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - m_startWall).count();
        try
        {
            detail::state().measurements.add(
                m_key, validCpu ? cpuEnd - m_startCpu : 0u,
                wall > 0 ? static_cast<uint64_t>(wall) : 0u, validCpu);
        }
        catch (...)
        {
            // Diagnostics must never replace the guest exception or alter its state.
        }
    }

private:
    CallKey m_key;
    bool m_sampled = false;
    bool m_hasCpu = false;
    uint64_t m_startCpu = 0u;
    std::chrono::steady_clock::time_point m_startWall{};
};

inline void onField(uint64_t field) noexcept
{
    if (!detail::enabled())
        return;
    try
    {
        auto &s = detail::state();
        if (s.firstField == 0u)
            detail::resetWindow(s, field);
        else if (field - s.firstField >= 120u)
        {
            detail::emit(s, field);
            detail::resetWindow(s, field);
        }
    }
    catch (...)
    {
        // Never disrupt VBlank delivery for optional logging.
    }
}

inline void finish(uint64_t field) noexcept
{
    if (!detail::enabled())
        return;
    try
    {
        auto &s = detail::state();
        detail::emit(s, field);
        detail::resetWindow(s, 0u);
    }
    catch (...)
    {
    }
}
#endif
} // namespace ps2x::ee_profile
