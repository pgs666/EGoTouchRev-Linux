// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace himax {

// No libsystemd dependency. The watchdog also runs while the panel is off;
// a stuck solver stops these notifications and systemd destroys its uinput fd.
inline void NotifyService(const char* message) {
    const char* path = std::getenv("NOTIFY_SOCKET");
    if (!path || (path[0] != '/' && path[0] != '@')) return;
    sockaddr_un address{};
    const size_t length = std::strlen(path);
    if (length >= sizeof(address.sun_path)) return;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path, length + 1);
    if (path[0] == '@') address.sun_path[0] = '\0';
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return;
    sendto(fd, message, std::strlen(message), MSG_NOSIGNAL,
           reinterpret_cast<sockaddr*>(&address),
           offsetof(sockaddr_un, sun_path) + length + (path[0] != '@'));
    close(fd);
}

}  // namespace himax
