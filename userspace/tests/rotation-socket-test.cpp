// SPDX-License-Identifier: MIT
#include "screen-rotation.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <systemd/sd-login.h>
#include <unistd.h>

static void Check(bool pass, const char* message) {
    if (!pass) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

int main() {
    // Independent of the production daemon and parallel test invocations.
    const std::string name = "himax-rotation-test-" + std::to_string(getpid());
    unsetenv("HIMAX_SCREEN_ROTATION_FILE");
    himax::ScreenRotationProvider provider({}, name);
    const int sender = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    Check(sender >= 0, "create datagram sender");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path + 1, name.data(), name.size());
    const auto length = offsetof(sockaddr_un, sun_path) + name.size() + 1;
    const auto send = [&](std::string_view message) {
        Check(sendto(sender, message.data(), message.size(), 0,
                     reinterpret_cast<sockaddr*>(&address), length) ==
                  static_cast<ssize_t>(message.size()), "send real datagram");
    };
    Check(!provider.Poll(), "empty nonblocking receiver");
    uid_t activeUid = 0;
    const bool activeSeat = sd_seat_get_active("seat0", nullptr, &activeUid) >= 0;
    const bool authorized = getuid() == 0 || (activeSeat && activeUid == getuid());
    send("kscreen=8\n");
    auto sample = provider.Poll();
    if (authorized) {
        Check(sample && sample->value == 3 && sample->source == "desktop-bridge",
              "kernel credential and KScreen message accepted");
        send("rotation=90");
        send("kscreen=4");
        sample = provider.Poll();
        Check(sample && sample->value == 2, "latest valid message wins");
        send("kscreen=16");
        send(std::string(128, '3'));
        send(std::string("3\0junk", 6));
        Check(!provider.Poll(), "reflected/truncated/embedded-NUL datagrams rejected");
        // Keep below Linux's default max_dgram_qlen; no sysctl changes needed.
        for (int i = 0; i < 8; ++i) send("1");
        sample = provider.Poll();
        Check(sample && sample->value == 1, "queued batch accepted");
        Check(!provider.Poll(), "batch fully drained");
    } else {
        Check(!sample, "inactive user rejected");
        std::cout << "No active seat for this UID; acceptance requires root/active-seat run\n";
    }
    // Running this test as root exercises genuine credential rejection;
    // sender cannot forge uid via the message payload.
    if (getuid() == 0) {
        const pid_t child = fork();
        Check(child >= 0, "fork unauthorized sender");
        if (!child) {
            const uid_t rejectedUid = activeUid == 65534 ? 65533 : 65534;
            if (setgid(rejectedUid) || setuid(rejectedUid)) _exit(2);
            send("kscreen=2");
            _exit(0);
        }
        int status = 0;
        Check(waitpid(child, &status, 0) == child &&
              WIFEXITED(status) && WEXITSTATUS(status) == 0, "unauthorized child sent");
        Check(!provider.Poll(), "kernel credentials reject unauthorized uid");
    }
    provider.SetFixed(0);
    send("3");
    sample = provider.Poll();
    Check(sample && sample->value == 0 && sample->source == "fixed",
          "fixed override wins over queued messages");
    close(sender);
    std::cout << "screen rotation socket regression passed\n";
}
