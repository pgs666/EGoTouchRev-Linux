// SPDX-License-Identifier: MIT
// Opt-in hardware test; deliberately not registered with CTest.
#include "stream-state.h"
#include "v4l2-capture.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <sys/ioctl.h>

namespace {
uint64_t Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Check(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

hx_raw_frame NextFrame(himax::Capture& capture, himax::StreamState& stream,
                       uint64_t afterEpoch) {
    const uint64_t deadline = Now() + 5000000000ULL;
    while (Now() < deadline) {
        hx_raw_state state{};
        while (capture.Event(state)) stream.State(state, Now());
        hx_raw_frame frame{};
        if (capture.Frame(frame) && stream.Accept(frame, Now()) &&
            frame.epoch > afterEpoch) return frame;
        Check(capture.Error().empty(), capture.Error());
        pollfd fd{capture.Fd(), POLLIN | POLLPRI, 0};
        const int result = poll(&fd, 1, 100);
        Check(result >= 0 || errno == EINTR, "poll: " + std::string(std::strerror(errno)));
        Check(!(fd.revents & (POLLERR | POLLHUP | POLLNVAL)), "transport stopped unexpectedly");
    }
    throw std::runtime_error("No fresh frame in the expected AFE epoch within five seconds");
}

void ExpectCommand(int fd, const hx_raw_frame& frame, uint32_t parameter, int expected) {
    hx_raw_command command{};
    command.size = sizeof(command);
    command.version = HX_RAW_ABI_VERSION;
    command.epoch = frame.epoch;
    command.sequence = frame.sequence;
    command.action = HX_RAW_RESET;
    command.parameter = parameter;
    const int result = ioctl(fd, VIDIOC_HIMAX_COMMAND, &command);
    const int error = result < 0 ? errno : 0;
    Check(error == expected, "reset command: expected errno=" + std::to_string(expected) +
          ", got=" + std::to_string(error));
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: himax-transport-test /dev/himax-raw\n"
                  << "Stop the touch service first; this test performs two AFE resets.\n";
        return 2;
    }
    try {
        himax::Capture capture;
        if (!capture.Open(argv[1]) || !capture.Start())
            throw std::runtime_error(capture.Error());
        himax::StreamState stream;
        const auto first = NextFrame(capture, stream, 0);
        ExpectCommand(capture.Fd(), first, 1, EINVAL);
        himax::Capture other;
        if (!other.Open(argv[1], false)) throw std::runtime_error(other.Error());
        ExpectCommand(other.Fd(), first, 0, EBUSY);

        // This stamp is taken before ioctl, so elapsed time here is an upper
        // bound on the interval seen by the kernel's cross-epoch limiter.
        const uint64_t resetAt = Now();
        ExpectCommand(capture.Fd(), first, 0, 0);
        const auto restarted = NextFrame(capture, stream, first.epoch);
        if (!capture.RefreshInfo()) throw std::runtime_error(capture.Error());
        const uint64_t elapsed = Now() - resetAt;
        Check(elapsed < 900000000ULL,
              "AFE took too long to verify the sub-second reset rejection; test inconclusive");
        ExpectCommand(capture.Fd(), first, 0, ESTALE);
        ExpectCommand(capture.Fd(), restarted, 0, EAGAIN);
        std::cout << "Cross-epoch reset rejected at " << elapsed / 1000000ULL << " ms\n";

        // Keep draining the queue while waiting; use a current frame for the
        // successful command so the eight-frame freshness rule is exercised.
        hx_raw_frame current{};
        do {
            current = NextFrame(capture, stream, first.epoch);
        } while (Now() - resetAt < 1100000000ULL);
        ExpectCommand(capture.Fd(), current, 0, 0);
        const auto second = NextFrame(capture, stream, restarted.epoch);
        int type = V4L2_BUF_TYPE_META_CAPTURE;
        Check(ioctl(capture.Fd(), VIDIOC_STREAMOFF, &type) == 0, "STREAMOFF failed");
        ExpectCommand(capture.Fd(), second, 0, ESTALE);
        std::cout << "Transport owner, parameter, epoch, reset interval and STREAMOFF tests passed: "
                  << first.epoch << " -> " << restarted.epoch << " -> " << second.epoch << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
