// Streaming VIF1 MFIFO consumer, adapted from the port's reversible runtime patch.
bool PS2Memory::tryProcessMfifoVif1(uint32_t channelBase, uint32_t chcr, bool restart)
{
    if (channelBase != 0x10009000u || ((chcr >> 2u) & 3u) != 1u)
        return false;
    const auto dctrlIt = m_ioRegisters.find(0x1000E000u);
    if (dctrlIt == m_ioRegisters.end() || ((dctrlIt->second >> 2u) & 3u) != 2u)
        return false;
    const uint32_t base = m_ioRegisters[0x1000E050u] & 0x7FFFFFF0u;
    const uint32_t size = (m_ioRegisters[0x1000E040u] & 0x7FFFFFF0u) + 16u;
    if (size < 16u || (size & (size - 1u)) != 0u || base >= PS2_RAM_SIZE || size > PS2_RAM_SIZE - base)
        throw std::runtime_error("Invalid VIF1 MFIFO ring range");
    const uint32_t mask = size - 1u;
    const auto wrap = [&](uint32_t addr) { return base + ((addr - base) & mask); };
    const auto available = [&](uint32_t addr) {
        const uint32_t writer = m_ioRegisters[0x1000D010u] & 0x7FFFFFF0u;
        return ((writer - addr) & mask) / 16u;
    };
    const auto empty = [&]() {
        vif1_regs.stat &= ~0x1F000000u; // VIF1 FQC has no ring data available.
        latchDmacStatus(14u);
    };
    const auto consume = [&](const uint8_t *data, uint32_t bytes) {
        m_vif1DmaStallRequested = false;
        m_vif1DmaProcessing = true;
        uint32_t consumed = 0u;
        try
        {
            consumed = processVIF1DataImpl(data, bytes, false, false);
        }
        catch (...)
        {
            m_vif1DmaProcessing = false;
            m_vif1DmaStallRequested = false;
            throw;
        }
        m_vif1DmaProcessing = false;
        m_vif1DmaStallRequested = false;
        return consumed;
    };
    if (restart)
        m_mfifoVif1 = {};
    for (uint32_t step = 0u; step < 4096u; ++step)
    {
        auto &state = m_mfifoVif1;
        if ((vif1_regs.stat & (1u << 10u)) != 0u) // VIS resumes only through STC.
            return true;
        if (!state.pending)
        {
            const uint32_t tagAddr = wrap(m_ioRegisters[channelBase + 0x30u]);
            m_ioRegisters[channelBase + 0x30u] = tagAddr;
            if (available(tagAddr) == 0u)
            {
                empty();
                return true; // Keep STR and TADR while waiting for the producer.
            }
            uint64_t tag = 0u;
            std::memcpy(&tag, m_rdram + tagAddr, sizeof(tag));
            const uint32_t qwc = static_cast<uint32_t>(tag & 0xFFFFu);
            const uint32_t id = static_cast<uint32_t>((tag >> 28u) & 7u);
            const uint32_t address = static_cast<uint32_t>(tag >> 32u) & 0xFFFFFFF0u;
            const uint32_t afterTag = wrap(tagAddr + 16u);
            state.pending = true;
            state.remaining = qwc;
            state.payloadOffset = 0u;
            state.ringPayload = id != 0u && id != 3u && id != 4u;
            state.dataAddr = state.ringPayload ? afterTag : dmaMemoryAddress(address);
            state.nextTag = state.ringPayload ? wrap(afterTag + qwc * 16u) : afterTag;
            state.end = id == 0u || id == 7u || (((tag >> 31u) & 1u) != 0u && (chcr & 0x80u) != 0u);
            uint32_t asp = (chcr >> 4u) & 3u;
            if (id == 2u)
                state.nextTag = wrap(address);
            else if (id == 5u)
            {
                if (asp >= 2u)
                    throw std::runtime_error("VIF1 MFIFO CALL stack overflow");
                m_ioRegisters[channelBase + 0x40u + asp * 16u] = state.nextTag;
                ++asp;
                state.nextTag = wrap(address);
            }
            else if (id == 6u)
            {
                if (asp == 0u)
                    state.end = true;
                else
                {
                    --asp;
                    state.nextTag = wrap(m_ioRegisters[channelBase + 0x40u + asp * 16u]);
                }
            }
            chcr = (chcr & 0x0000FFCFu) | (asp << 4u) | (static_cast<uint32_t>((tag >> 16u) & 0xFFFFu) << 16u);
            m_ioRegisters[channelBase] = chcr;
            m_ioRegisters[channelBase + 0x10u] = state.dataAddr;
            m_ioRegisters[channelBase + 0x20u] = qwc;
            m_ioRegisters[channelBase + 0x30u] = afterTag;
            // An exhausted ring does not prevent external REF-family data
            // from running. It still latches MFIFO Empty before that payload.
            if (qwc != 0u && available(afterTag) == 0u)
                empty();
            state.tagBytes = (chcr & 0x40u) != 0u ? 8u : 0u;
            if (state.tagBytes != 0u)
                std::memcpy(&state.tagData, m_rdram + tagAddr + 8u, sizeof(state.tagData));
        }
        if (state.tagBytes != 0u)
        {
            const auto *data = reinterpret_cast<const uint8_t *>(&state.tagData);
            const uint32_t count = state.tagBytes;
            const uint32_t consumed = consume(data + 8u - count, count);
            state.tagBytes -= consumed;
            if (consumed < count || (vif1_regs.stat & (1u << 10u)) != 0u)
                return true;
        }
        if (state.remaining != 0u)
        {
            uint32_t count = state.remaining;
            if (state.ringPayload)
            {
                count = std::min(count, available(state.dataAddr));
                count = std::min(count, (base + size - state.dataAddr) / 16u);
                if (count == 0u)
                {
                    empty();
                    return true;
                }
            }
            const bool scratch = !state.ringPayload && isScratchpad(state.dataAddr);
            const uint32_t offset = translateAddress(state.dataAddr) + state.payloadOffset;
            const uint32_t capacity = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            uint32_t bytes = count * 16u - state.payloadOffset;
            if (offset >= capacity || (!scratch && bytes > capacity - offset))
                throw std::runtime_error("VIF1 MFIFO payload outside DMA memory");
            bytes = std::min(bytes, capacity - offset);
            const uint32_t consumed = consume((scratch ? m_scratchpad : m_rdram) + offset, bytes);
            const uint32_t progress = state.payloadOffset + consumed;
            const uint32_t advance = (progress / 16u) * 16u;
            state.payloadOffset = progress & 15u;
            state.remaining -= progress / 16u;
            if (state.ringPayload)
                state.dataAddr = wrap(state.dataAddr + advance);
            else if (scratch)
                state.dataAddr = PS2_SCRATCHPAD_BASE + ((state.dataAddr + advance) & (PS2_SCRATCHPAD_SIZE - 1u));
            else
                state.dataAddr += advance;
            m_ioRegisters[channelBase + 0x10u] = state.dataAddr;
            m_ioRegisters[channelBase + 0x20u] = state.remaining;
            if (state.ringPayload)
            {
                m_ioRegisters[channelBase + 0x30u] = state.dataAddr;
                if (available(state.dataAddr) == 0u)
                    empty();
            }
            if (consumed < bytes || (vif1_regs.stat & (1u << 10u)) != 0u)
                return true;
            if (state.remaining != 0u)
                continue;
        }
        const bool done = state.end;
        m_ioRegisters[channelBase + 0x30u] = state.nextTag;
        state.pending = false;
        if (done)
        {
            if (available(state.nextTag) == 0u)
                empty();
            completeDmacChannel(channelBase, 1u);
            return true;
        }
    }
    return true;
}
