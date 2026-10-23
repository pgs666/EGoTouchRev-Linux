// SPDX-License-Identifier: MIT
#include "control.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace himax {

Control::Control() {
    const int directory = open(kDirectory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat info{};
    if (directory < 0 || fstat(directory, &info) || info.st_uid != 0 || (info.st_mode & 0077)) {
        if (directory >= 0) close(directory);
        m_error = "Control requires root-owned /run/himax-touchd with mode 0700";
        return;
    }
    // Only remove our stale socket in the protected runtime directory.
    if (fstatat(directory, "control.sock", &info, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISSOCK(info.st_mode) || info.st_uid != 0 || unlinkat(directory, "control.sock", 0)) {
            close(directory);
            m_error = "Cannot remove stale control socket";
            return;
        }
    } else if (errno != ENOENT) {
        close(directory);
        m_error = "Cannot inspect control socket";
        return;
    }
    close(directory);
    m_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strcpy(address.sun_path, kPath);
    if (m_fd < 0 || bind(m_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address))) {
        m_error = std::string("Control bind: ") + std::strerror(errno);
        if (m_fd >= 0) close(m_fd);
        m_fd = -1;
        return;
    }
    m_bound = true;
}

Control::~Control() {
    if (m_fd >= 0) close(m_fd);
    if (m_bound) unlink(kPath);
}

bool Control::Receive(Command& command) {
    std::array<char, 32> bytes{};
    m_peer = {};
    m_peerLength = sizeof(m_peer);
    // recvfrom does not import ancillary file descriptors. Parent directory
    // mode 0700 supplies access control, including for requests from abstract
    // return addresses; no per-frame credential lookup is necessary.
    const auto length = recvfrom(m_fd, bytes.data(), bytes.size(), MSG_DONTWAIT | MSG_TRUNC,
        reinterpret_cast<sockaddr*>(&m_peer), &m_peerLength);
    if (length < 0) return false;
    command = Command::Invalid;
    if (length > 0 && size_t(length) <= bytes.size()) {
        const std::string_view text(bytes.data(), size_t(length));
        if (text == "status") command = Command::Status;
        else if (text == "calibrate") command = Command::Calibrate;
        else if (text == "reset") command = Command::Reset;
        else if (text == "wake") command = Command::Wake;
    }
    return true;
}

void Control::Reply(std::string_view text) const {
    if (m_peerLength <= offsetof(sockaddr_un, sun_path) || m_peerLength > sizeof(m_peer)) return;
    (void)sendto(m_fd, text.data(), text.size(), MSG_DONTWAIT | MSG_NOSIGNAL,
                 reinterpret_cast<const sockaddr*>(&m_peer), m_peerLength);
}

void Control::Operation::Sent(uint64_t newEpoch, uint64_t newSequence, uint64_t now) {
    epoch = newEpoch;
    sequence = newSequence;
    sentNs = now;
    pending = true;
    state = "pending";
}

void Control::Operation::Failed() {
    pending = false;
    state = "failed";
}

bool Control::Operation::Stop() {
    if (!pending) return false;
    pending = false;
    state = "interrupted";
    return true;
}

bool Control::Operation::Frame(uint64_t frameEpoch, uint64_t frameSequence, uint64_t sampleNs, bool done) {
    if (!pending) return false;
    if (frameEpoch != epoch) return Stop();
    // The frame-bound ioctl permits a small producer lead. A queued frame
    // can therefore have a newer sequence while predating this command.
    if (!done || frameSequence <= sequence || sampleNs < sentNs) return false;
    pending = false;
    state = "done";
    return true;
}

bool Control::Operation::Tick(uint64_t now) {
    if (!pending || now < sentNs || now - sentNs < 8000000000ULL) return false;
    pending = false;
    state = "timeout";
    return true;
}

} // namespace himax
