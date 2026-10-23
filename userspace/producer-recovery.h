// SPDX-License-Identifier: MIT
#pragma once

#include "stream-state.h"
#include <cerrno>

namespace himax {

// Master-only adaptation of Windows GetFrame's 200 ms wait, IDLE retries,
// 20 ms rearm grace, seven reopens and 3200 ms budget. No solver/AFE reset here.
// Polling the device and receiving another valid frame remain separate from
// successfully issuing a rearm ioctl. Old modules keep their four-second path.
class ProducerRecovery {
public:
    enum class Step { None, Rearm, Exhausted };

    Step Next(const StreamState& stream, uint64_t now, bool idle) {
        if (!stream.Running()) {
            m_epoch = m_progress = m_sequence = m_lastAttempt = 0;
            m_attempts = 0;
            return Step::None;
        }
        if (m_epoch != stream.Epoch()) {
            m_attempts = 0;
            m_lastAttempt = 0;
        }
        if (m_epoch != stream.Epoch() || m_sequence != stream.Sequence() ||
            m_progress != stream.ProgressNs()) {
            m_epoch = stream.Epoch();
            m_sequence = stream.Sequence();
            m_progress = stream.ProgressNs();
            m_lastAttempt = 0;
            // Delivery ends the previous wait, but an all-zero/checksum-bad
            // packet cannot forgive the accumulated rearm attempts.
        }
        if (m_unsupported)
            return stream.TimedOut(now) ? Step::Exhausted : Step::None;
        if (now <= m_progress) return Step::None;
        const auto quiet = now - m_progress;
        if (quiet < (idle ? 3600000000ULL : 200000000ULL)) return Step::None;
        if (m_attempts >= 7 && !m_lastAttempt) return Step::Exhausted;
        // Always allow the first kick, including after the IDLE retry window
        // already consumed the 3.2-second Windows per-call budget.
        if (m_lastAttempt) {
            if (now < m_lastAttempt || now - m_lastAttempt < 20000000ULL)
                return Step::None;
            if (m_attempts >= 7 || m_lastAttempt - m_progress > 3200000000ULL)
                return Step::Exhausted;
            if (now - m_lastAttempt < 220000000ULL) return Step::None;
        }
        ++m_attempts;
        m_lastAttempt = now;
        return Step::Rearm;
    }

    void Result(int error) {
        if (error == ENOTTY) m_unsupported = true;
    }

    // Windows clears read stalls after frame validity, not merely a successful
    // transport. In particular an IDLE kick can legitimately return all zeros.
    void Frame(bool valid) {
        if (valid) {
            m_attempts = 0;
            m_lastAttempt = 0;
        }
    }

private:
    uint64_t m_epoch = 0, m_progress = 0, m_sequence = 0, m_lastAttempt = 0;
    unsigned m_attempts = 0;
    bool m_unsupported = false;
};

} // namespace himax
