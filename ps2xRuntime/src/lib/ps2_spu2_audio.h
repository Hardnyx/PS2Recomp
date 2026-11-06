// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ps2_host_backend.h"

class PS2Spu2Audio
{
public:
    PS2Spu2Audio();
    ~PS2Spu2Audio();
    PS2Spu2Audio(const PS2Spu2Audio &) = delete;
    PS2Spu2Audio &operator=(const PS2Spu2Audio &) = delete;

    bool active() const noexcept { return m_active; }

private:
    AudioStream m_stream{};
    bool m_active = false;
};
