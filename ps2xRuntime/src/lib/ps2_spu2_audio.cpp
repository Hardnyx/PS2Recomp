// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps2_spu2_audio.h"

#include "kzspu2.h"
#include "ps2x/iop/iop_host_spu2.h"

#include <algorithm>

namespace
{
    constexpr unsigned kSampleRate = 48'000;
    constexpr unsigned kChannels = 2;
    constexpr unsigned kStreamBufferFrames = 512;

    void fillStream(void *buffer, unsigned int frames)
    {
        auto *samples = static_cast<int16_t *>(buffer);
        const uint32_t read = kzspu2ReadSamples(samples, frames);
        std::fill(samples + kChannels * read, samples + kChannels * frames, int16_t{0});
    }

    void startDma(int core, uint16_t *ram, uint32_t madr, uint32_t halfwords, bool toSpu2, uint64_t cycle)
    {
        if (toSpu2)
            kzspu2DmaWrite(core, ram, halfwords, madr, cycle);
        else
            kzspu2DmaRead(core, ram, halfwords, madr, cycle);
    }
}

PS2Spu2Audio::PS2Spu2Audio()
{
    const bool deviceReady = IsAudioDeviceReady();
    KzSpu2Config config;
    config.ringFrames = deviceReady ? 4096u : 0u;
    config.irq = [](void *) { ps2x::iop::iopSpu2RaiseIrq(); };
    config.dmaComplete = [](void *, int core) { ps2x::iop::iopSpu2DmaComplete(core); };
    if (!kzspu2Init(config))
        return;

    m_active = true;
    ps2x::iop::IopHostSpu2 hooks;
    hooks.read16 = kzspu2Read16;
    hooks.write16 = kzspu2Write16;
    hooks.dmaStart = startDma;
    hooks.dmaMadr = kzspu2DmaMadr;
    hooks.advance = kzspu2Advance;
    hooks.nextEvent = kzspu2NextEventCycle;
    ps2x::iop::iopSetHostSpu2(hooks);

    if (deviceReady)
    {
        SetAudioStreamBufferSizeDefault(kStreamBufferFrames);
        m_stream = LoadAudioStream(kSampleRate, 16, kChannels);
        SetAudioStreamBufferSizeDefault(0);
        if (m_stream.buffer)
        {
            SetAudioStreamCallback(m_stream, fillStream);
            PlayAudioStream(m_stream);
        }
    }
}

PS2Spu2Audio::~PS2Spu2Audio()
{
    if (!m_active)
        return;
    if (m_stream.buffer)
    {
        StopAudioStream(m_stream);
        UnloadAudioStream(m_stream);
    }
    ps2x::iop::iopSetHostSpu2({});
    kzspu2Shutdown();
}
