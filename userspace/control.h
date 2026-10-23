// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>
#include <cstdint>
#include <sys/socket.h>
#include <sys/un.h>

namespace himax {

// Single-threaded operator control. The systemd RuntimeDirectory is root-only;
// no worker, register access, or additional V4L2 owner is introduced.
class Control {
public:
    static constexpr const char* kDirectory = "/run/himax-touchd";
    static constexpr const char* kPath = "/run/himax-touchd/control.sock";
    enum class Command { Status, Calibrate, Reset, Wake, Invalid };

    Control();
    ~Control();
    Control(const Control&) = delete;
    Control& operator=(const Control&) = delete;
    bool Receive(Command& command);
    void Reply(std::string_view text) const;
    int Fd() const { return m_fd; }
    const std::string& Error() const { return m_error; }
    const char* CalibrationState() const { return m_calibration.state; }
    bool CalibrationPending() const { return m_calibration.pending; }
    void CalibrationSent(uint64_t epoch, uint64_t sequence, uint64_t now) {
        m_calibration.Sent(epoch, sequence, now);
    }
    void CalibrationFailed() { m_calibration.Failed(); }
    bool CalibrationFrame(uint64_t epoch, uint64_t sequence, uint64_t sampleNs, bool done) {
        return m_calibration.Frame(epoch, sequence, sampleNs, done);
    }
    const char* WakeState() const { return m_wake.state; }
    bool WakePending() const { return m_wake.pending; }
    void WakeSent(uint64_t epoch, uint64_t sequence, uint64_t now) {
        m_wake.Sent(epoch, sequence, now);
    }
    void WakeFailed() { m_wake.Failed(); }
    bool WakeFrame(uint64_t epoch, uint64_t sequence, uint64_t sampleNs, bool active) {
        return m_wake.Frame(epoch, sequence, sampleNs, active);
    }
    void AfeStopped() { m_calibration.Stop(); m_wake.Stop(); }
    bool Tick(uint64_t now) {
        const bool calibration = m_calibration.Tick(now);
        const bool wake = m_wake.Tick(now);
        return calibration || wake;
    }

private:
    int m_fd = -1;
    bool m_bound = false;
    sockaddr_un m_peer{};
    socklen_t m_peerLength = 0;
    std::string m_error;
    struct Operation {
        const char* state = "none";
        bool pending = false;
        uint64_t epoch = 0, sequence = 0, sentNs = 0;
        void Sent(uint64_t newEpoch, uint64_t newSequence, uint64_t now);
        void Failed();
        bool Stop();
        bool Frame(uint64_t frameEpoch, uint64_t frameSequence, uint64_t sampleNs, bool done);
        bool Tick(uint64_t now);
    };
    Operation m_calibration, m_wake;
};

} // namespace himax
