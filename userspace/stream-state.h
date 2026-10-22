// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include "hx-v4l2-uapi.h"

namespace himax {

// V4L2 events and completed buffers have separate queues. A panel-off event
// must invalidate already completed frames, even when they are read later.
class StreamState {
public:
    bool State(const hx_raw_state& state) {
        if (state.epoch < m_epoch) return false;
        m_epoch = state.epoch;
        m_running = state.running;
        return true;
    }

    bool Accept(const hx_raw_frame& frame, uint64_t now) {
        if (!frame.epoch || !frame.sequence || frame.epoch < m_epoch ||
            (frame.epoch == m_epoch && !m_running) ||
            frame.timestamp_ns > now || now - frame.timestamp_ns > 50000000)
            return false;
        if (frame.epoch == m_frameEpoch &&
            (frame.sequence <= m_sequence || frame.timestamp_ns < m_timestamp))
            return false;
        m_epoch = m_frameEpoch = frame.epoch;
        m_running = true;
        m_sequence = frame.sequence;
        m_timestamp = frame.timestamp_ns;
        return true;
    }

private:
    uint64_t m_epoch = 0;
    uint64_t m_frameEpoch = 0;
    uint64_t m_sequence = 0;
    uint64_t m_timestamp = 0;
    bool m_running = false;
};

}  // namespace himax
