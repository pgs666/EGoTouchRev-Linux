// SPDX-License-Identifier: MIT
#pragma once

#include "native-frame.h"
#include "hx-v4l2-uapi.h"

namespace himax {

// Windows ArmIdleRetry controls read waiting independently of the firmware's
// reported AFE mode. Sending EnterIdle successfully arms this hint immediately;
// it is not an acknowledgement that the controller has entered IDLE.
// Finger-only: no slave read or stylus lifecycle is enabled by this helper.
class FrameReadMode {
public:
    void Reset() noexcept { m_idleRetry = false; }

    void Action(hx_raw_action action, bool succeeded) noexcept {
        if (succeeded && action == HX_RAW_ENTER_IDLE) m_idleRetry = true;
    }

    void Observe(FrameError error,
                 const std::array<uint8_t, Frame::kMasterFrameSize>& thp,
                 bool rearm = false) noexcept {
        // Intentional Linux no-data exception. Never inspect the previous
        // successful THP buffer when conversion of this packet failed.
        if (IsEmptyRearm(error, rearm)) return;
        if (error != FrameError::None) {
            Reset();
            return;
        }
        constexpr size_t offset =
            Frame::kMasterSuffixOffset + 2 * Frame::MasterWord::kIdleFlag;
        const uint16_t idle = static_cast<uint16_t>(thp[offset] | (thp[offset + 1] << 8));
        if (idle == 1) m_idleRetry = true;
        if (idle == 0) m_idleRetry = false;
        // Other suffix values preserve the prior hint, as in GetFrame.
    }

    [[nodiscard]] bool IdleRetry() const noexcept { return m_idleRetry; }

private:
    bool m_idleRetry = false;
};

} // namespace himax
