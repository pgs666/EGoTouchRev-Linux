// SPDX-License-Identifier: MIT
#include "screen-rotation.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
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
    const std::string bridgePrefix = "himax-rotation-request-test-" + std::to_string(getpid()) + "-";
    unsetenv("HIMAX_SCREEN_ROTATION_FILE");
    himax::ScreenRotationProvider provider({}, name, bridgePrefix);
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
    Check(!provider.RequestInitial(), "missing bridge fails without waiting");
    const int bridge = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    Check(bridge >= 0, "create startup request server");
    int passCredentials = 1;
    Check(!setsockopt(bridge, SOL_SOCKET, SO_PASSCRED, &passCredentials, sizeof(passCredentials)),
          "request server enables kernel credentials");
    sockaddr_un bridgeAddress{};
    bridgeAddress.sun_family = AF_UNIX;
    const auto bridgeName = bridgePrefix + std::to_string(activeSeat ? activeUid : getuid());
    std::memcpy(bridgeAddress.sun_path + 1, bridgeName.data(), bridgeName.size());
    Check(!bind(bridge, reinterpret_cast<sockaddr*>(&bridgeAddress),
                offsetof(sockaddr_un, sun_path) + 1 + bridgeName.size()),
          "bind unique active-user request endpoint");
    Check(provider.RequestInitial(), "initial direction requested");
    std::array<char, 64> request{};
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(ucred))> requestControl{};
    sockaddr_un peer{};
    iovec requestData{request.data(), request.size()};
    msghdr requestMessage{};
    requestMessage.msg_name = &peer;
    requestMessage.msg_namelen = sizeof(peer);
    requestMessage.msg_iov = &requestData;
    requestMessage.msg_iovlen = 1;
    requestMessage.msg_control = requestControl.data();
    requestMessage.msg_controllen = requestControl.size();
    const auto received = recvmsg(bridge, &requestMessage, MSG_DONTWAIT);
    Check(received == static_cast<ssize_t>(himax::kScreenRotationRequest.size()) &&
          std::string_view(request.data(), received) == himax::kScreenRotationRequest,
          "exact versioned startup request");
    Check(!requestMessage.msg_flags && requestMessage.msg_namelen == length &&
          !std::memcmp(&peer, &address, length), "request originates at provider endpoint");
    auto* credentialsHeader = CMSG_FIRSTHDR(&requestMessage);
    Check(credentialsHeader && credentialsHeader->cmsg_level == SOL_SOCKET &&
          credentialsHeader->cmsg_type == SCM_CREDENTIALS &&
          credentialsHeader->cmsg_len == CMSG_LEN(sizeof(ucred)),
          "request has kernel credential message");
    ucred credentials{};
    std::memcpy(&credentials, CMSG_DATA(credentialsHeader), sizeof(credentials));
    Check(credentials.uid == getuid() && credentials.pid == getpid(),
          "request uses real process credentials");
    Check(sendto(bridge, "kscreen=8\n", 10, 0, reinterpret_cast<sockaddr*>(&peer),
                 requestMessage.msg_namelen) == 10, "mock bridge replies from cache");
    auto initial = provider.Poll();
    Check(authorized ? initial && initial->value == 3 : !initial,
          "initial response uses normal credential validation");
    provider.SetFixed(0);
    Check(!provider.RequestInitial(), "fixed direction suppresses startup request");
    provider.SetFixed(std::nullopt);
    himax::ScreenRotationProvider fileProvider("/nonexistent/himax-rotation", {}, bridgePrefix);
    Check(!fileProvider.RequestInitial(), "explicit file suppresses startup request");
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
    // Cover both fully delivered and truncated SCM_RIGHTS. Reject the packet
    // and close all descriptors installed by recvmsg, regardless of UID.
    const auto fdCount = [] {
        size_t count = 0;
        for ([[maybe_unused]] const auto& entry :
             std::filesystem::directory_iterator("/proc/self/fd")) ++count;
        return count;
    };
    const int descriptor = open("/dev/null", O_RDONLY | O_CLOEXEC);
    Check(descriptor >= 0, "open descriptor for unsolicited rights test");
    for (const size_t count : {1u, 16u}) {
        alignas(cmsghdr) std::array<char, CMSG_SPACE(16 * sizeof(int))> control{};
        iovec payload{const_cast<char*>("kscreen=2\n"), 10};
        msghdr message{};
        message.msg_name = &address;
        message.msg_namelen = length;
        message.msg_iov = &payload;
        message.msg_iovlen = 1;
        message.msg_control = control.data();
        message.msg_controllen = CMSG_SPACE(count * sizeof(int));
        auto* rights = CMSG_FIRSTHDR(&message);
        rights->cmsg_level = SOL_SOCKET;
        rights->cmsg_type = SCM_RIGHTS;
        rights->cmsg_len = CMSG_LEN(count * sizeof(int));
        for (size_t i = 0; i < count; ++i)
            std::memcpy(CMSG_DATA(rights) + i * sizeof(int), &descriptor, sizeof(int));
        const size_t before = fdCount();
        Check(sendmsg(sender, &message, MSG_DONTWAIT) == 10, "send unsolicited descriptors");
        Check(!provider.Poll(), "unsolicited descriptors rejected");
        Check(fdCount() == before, "no received descriptor leak");
    }
    close(descriptor);
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
    close(bridge);
    std::cout << "screen rotation socket regression passed\n";
}
