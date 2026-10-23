// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>
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

private:
    int m_fd = -1;
    bool m_bound = false;
    sockaddr_un m_peer{};
    socklen_t m_peerLength = 0;
    std::string m_error;
};

} // namespace himax
