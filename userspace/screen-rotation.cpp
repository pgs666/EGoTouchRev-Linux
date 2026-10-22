// SPDX-License-Identifier: MIT
#include "screen-rotation.h"

#include <array>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <systemd/sd-login.h>
#include <unistd.h>
#include <utility>

namespace himax {
namespace {

std::string_view Trim(std::string_view text) noexcept {
    constexpr std::string_view spaces = " \t\r\n";
    const auto first = text.find_first_not_of(spaces);
    if (first == text.npos) return {};
    const auto last = text.find_last_not_of(spaces);
    return text.substr(first, last - first + 1);
}

std::optional<int> Integer(std::string_view text) noexcept {
    if (text.empty()) return {};
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return {};
    return value;
}

std::optional<int> ParseSample(std::string_view text) {
    text = Trim(text);
    if (text.starts_with("kscreen=")) {
        const auto mask = Integer(text.substr(8));
        return mask ? ParseKScreenRotation(*mask) : std::nullopt;
    }
    return ParseScreenRotation(text);
}

std::optional<ScreenRotationSample> ReadFile(const std::string& path) {
    // An accidental FIFO/device must not block the input thread. Bound the
    // read even for a user-supplied path; never allocate based on file length.
    const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return {};
    struct stat info {};
    std::array<char, 64> buffer{};
    ssize_t length = -1;
    if (!fstat(fd, &info) && S_ISREG(info.st_mode) &&
        info.st_size > 0 &&
        info.st_size < static_cast<off_t>(buffer.size()))
        length = read(fd, buffer.data(), buffer.size());
    close(fd);
    if (length <= 0 || length >= static_cast<ssize_t>(buffer.size())) return {};
    const auto value = ParseSample(std::string_view(buffer.data(), length));
    if (!value) return {};
    return ScreenRotationSample{*value, path};
}

}  // namespace

std::optional<int> ParseScreenRotation(std::string_view text) noexcept {
    text = Trim(text);
    if (text.starts_with("rotation=")) text = Trim(text.substr(9));
    const auto value = Integer(text);
    if (!value) return {};
    if (*value >= 0 && *value <= 3) return value;
    switch (*value) {
    case 90: return 1;
    case 180: return 2;
    case 270: return 3;
    default: return {};
    }
}

std::optional<int> ParseKScreenRotation(int mask) noexcept {
    // libkscreen Output::Rotation. Reflections are not rotations and must not
    // silently select a grip profile. See docs/touch-offload.md.
    switch (mask) {
    case 1: return 0;
    case 2: return 1;
    case 4: return 2;
    case 8: return 3;
    default: return {};
    }
}

ScreenRotationProvider::ScreenRotationProvider(std::string rotationFile, std::string_view socketName)
    : m_rotationFile(std::move(rotationFile)) {
    if (m_rotationFile.empty()) {
        if (const char* env = std::getenv("HIMAX_SCREEN_ROTATION_FILE"); env && *env)
            m_rotationFile = env;
    }
    if (!m_rotationFile.empty() || socketName.empty() ||
        socketName.size() >= sizeof(sockaddr_un::sun_path)) return;
    m_socket = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (m_socket < 0) return;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path + 1, socketName.data(), socketName.size());
    int passCredentials = 1;
    if (setsockopt(m_socket, SOL_SOCKET, SO_PASSCRED, &passCredentials, sizeof(passCredentials)) ||
        bind(m_socket, reinterpret_cast<sockaddr*>(&address),
             offsetof(sockaddr_un, sun_path) + 1 + socketName.size())) {
        close(m_socket);
        m_socket = -1;
    }
}

ScreenRotationProvider::~ScreenRotationProvider() {
    if (m_socket >= 0) close(m_socket);
}

void ScreenRotationProvider::SetFixed(std::optional<int> fixedRotation) noexcept {
    m_fixed = fixedRotation;
}

std::optional<ScreenRotationSample> ScreenRotationProvider::Poll() const {
    if (m_fixed) return ScreenRotationSample{*m_fixed, "fixed"};
    if (!m_rotationFile.empty()) return ReadFile(m_rotationFile);
    uid_t uid = 0;
    const bool activeUser = sd_seat_get_active("seat0", nullptr, &uid) >= 0;
    std::optional<ScreenRotationSample> latest;
    // Bounded even if a local sender floods the socket. No filesystem, D-Bus
    // or external command is part of the per-frame algorithm path.
    for (unsigned int i = 0; m_socket >= 0 && i < 16; ++i) {
        std::array<char, 64> buffer{};
        alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(ucred))> control{};
        iovec data{buffer.data(), buffer.size()};
        msghdr message{};
        message.msg_iov = &data;
        message.msg_iovlen = 1;
        message.msg_control = control.data();
        message.msg_controllen = control.size();
        const auto length = recvmsg(m_socket, &message, MSG_DONTWAIT);
        if (length < 0) break;
        if (!length || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) continue;
        bool authorized = false;
        for (auto* cmsg = CMSG_FIRSTHDR(&message); cmsg; cmsg = CMSG_NXTHDR(&message, cmsg)) {
            if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_CREDENTIALS ||
                cmsg->cmsg_len != CMSG_LEN(sizeof(ucred))) continue;
            ucred credentials{};
            std::memcpy(&credentials, CMSG_DATA(cmsg), sizeof(credentials));
            authorized = credentials.uid == 0 || (activeUser && credentials.uid == uid);
        }
        if (authorized) {
            if (const auto value = ParseSample(std::string_view(buffer.data(), length)))
                latest = ScreenRotationSample{*value, "desktop-bridge"};
        }
    }
    return latest;
}

}  // namespace himax
