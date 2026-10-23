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
    enum class Command { Status, Calibrate, Reset, Invalid };

    Control();
    ~Control();
    Control(const Control&) = delete;
    Control& operator=(const Control&) = delete;
    bool Receive(Command& command);
    void Reply(std::string_view text) const;
    int Fd() const { return m_fd; }
    const std::string& Error() const { return m_error; }
    const char* CalibrationState() const { return m_calibrationState; }
    bool CalibrationPending() const { return m_calibrationPending; }
    void CalibrationSent(uint64_t epoch, uint64_t sequence, uint64_t now);
    void CalibrationFailed();
    bool CalibrationStopped();
    bool CalibrationFrame(uint64_t epoch, uint64_t sequence, bool done);
    bool Tick(uint64_t now);

private:
    int m_fd = -1;
    bool m_bound = false;
    sockaddr_un m_peer{};
    socklen_t m_peerLength = 0;
    std::string m_error;
    const char* m_calibrationState = "none";
    bool m_calibrationPending = false;
    uint64_t m_calibrationEpoch = 0, m_calibrationSequence = 0, m_calibrationSentNs = 0;
};

} // namespace himax
