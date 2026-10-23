// SPDX-License-Identifier: MIT
#pragma once

#include "stream-state.h"
#include <cerrno>

namespace himax {

// Linux host bounds around Windows-style same-process AFE reconstruction.
// No hardware/solver/output calls here. Caller uses the returned request token,
// records its result, then stops solver/input only after kernel acceptance.
class AfeRestart {
public:
    enum class Step { None, Request, Fallback };
    static constexpr uint64_t kRaceBudgetNs = 1000000000;
    static constexpr uint64_t kRetryNs = 100000000;
    static constexpr uint64_t kReadyBudgetNs = 15000000000;
    static constexpr uint64_t kStableNs = 30000000000;
    static constexpr unsigned kMaxAccepted = 3;

    Step Next(const StreamState& stream, uint64_t now, bool exhausted) {
        if (m_waiting) {
            if (now >= m_completed && now - m_completed > kReadyBudgetNs) m_failed = true;
            return m_failed ? Step::Fallback : Step::None;
        }
        if (m_failed) return Step::Fallback;
        if (!stream.Running() || !exhausted) {
            m_retrying = false;
            return Step::None;
        }
        if (m_retrying && (stream.Epoch() != m_epoch || stream.Sequence() != m_sequence ||
                          stream.ProgressNs() != m_progress)) {
            // Drain lifecycle/data before considering this new producer again.
            m_retrying = false;
            return Step::None;
        }
        if (m_accepted >= kMaxAccepted) return Step::Fallback;
        if (m_retrying) {
            if (now < m_began || now - m_began >= kRaceBudgetNs) return Step::Fallback;
            if (now < m_completed || now - m_completed < kRetryNs) return Step::None;
        } else {
            m_began = now;
            m_epoch = stream.Epoch();
            m_sequence = stream.Sequence();
            m_progress = stream.ProgressNs();
        }
        m_submitted = now;
        m_retrying = true;
        return Step::Request;
    }

    bool Result(int error, uint64_t completed) {
        if (completed < m_submitted) { m_failed = true; return false; }
        m_completed = completed;
        m_stableSince = 0;
        if (!error) {
            ++m_accepted;
            m_waiting = true;
            m_fenced = true;
            m_retrying = false;
            return true;
        }
        if (error != ESTALE && error != EAGAIN && error != EBUSY)
            m_failed = true; // ENOTTY retains the supervisor fallback.
        return false;
    }

    // Call only after raw AND AFE status validity. A successful ioctl/running
    // event/old packet/checksum-valid SOS packet cannot complete reconstruction.
    bool Frame(const hx_raw_frame& packet, uint64_t now, bool usable) {
        if (m_failed) return false;
        if (!usable) { m_stableSince = 0; return false; }
        if (m_fenced && packet.epoch <= m_epoch) return false;
        bool recovered = false;
        if (m_waiting) {
            if (packet.epoch <= m_epoch) return false;
            if (now < m_completed || now - m_completed > kReadyBudgetNs) {
                m_failed = true;
                return false;
            }
            m_waiting = false;
            recovered = true;
        }
        m_fenced = false;
        if (m_lastUsable && (now < m_lastUsable || now - m_lastUsable > 4000000000ULL))
            m_stableSince = 0;
        m_lastUsable = now;
        if (!m_stableSince) m_stableSince = now;
        if (now >= m_stableSince && now - m_stableSince >= kStableNs)
            m_accepted = 0;
        return recovered;
    }

    bool Allows(const hx_raw_frame& packet) const {
        return !m_fenced || packet.epoch > m_epoch;
    }
    bool Allows(const hx_raw_state& state) const {
        return !m_fenced || !state.running || state.epoch > m_epoch;
    }
    bool State(const hx_raw_state& state, uint32_t flags = 0) {
        if (!state.running) m_stableSince = 0;
        if (state.running || !(flags & HX_RAW_STOP_KNOWN) ||
            !(flags & HX_RAW_STOP_REASONS) || state.epoch < m_epoch) return false;
        // Windows EnterStandby cancels recovery and resets its failure budget.
        // Keep the accepted-request epoch fence until genuinely newer data.
        const bool interrupted = m_waiting || m_retrying;
        m_waiting = m_retrying = m_failed = false;
        m_accepted = 0;
        m_lastUsable = 0;
        return interrupted;
    }
    bool Waiting() const { return m_waiting; }
    uint64_t Epoch() const { return m_epoch; }
    uint64_t Sequence() const { return m_sequence; }

private:
    uint64_t m_epoch = 0, m_sequence = 0, m_progress = 0;
    uint64_t m_began = 0, m_submitted = 0, m_completed = 0, m_stableSince = 0, m_lastUsable = 0;
    unsigned m_accepted = 0;
    bool m_retrying = false, m_waiting = false, m_failed = false, m_fenced = false;
};

} // namespace himax
