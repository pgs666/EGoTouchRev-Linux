// SPDX-License-Identifier: MIT
// Explicit hands-off test: temporarily pauses only the userspace consumer.
// IRQ/SPI producers remain enabled. Never registered with CTest.
#include "recording.h"
#include "v4l2-capture.h"

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

namespace {
uint64_t Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void Check(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
void Allocation(const char* path) {
    struct File {
        int fd;
        ~File() { if (fd >= 0) close(fd); }
    } file{open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC)};
    Check(file.fd >= 0, "Cannot open allocation probe");
    hx_raw_info info{};
    Check(ioctl(file.fd, VIDIOC_HIMAX_INFO, &info) == 0 && info.size == sizeof(info) &&
          info.version == HX_RAW_ABI_VERSION && info.rows == 40 && info.cols == 60,
          "Allocation target is not expected Himax raw device");
    for (unsigned requested : {1u, 4u, 8u, 32u}) {
        v4l2_requestbuffers request{};
        request.type = V4L2_BUF_TYPE_META_CAPTURE;
        request.memory = V4L2_MEMORY_MMAP;
        request.count = requested;
        Check(ioctl(file.fd, VIDIOC_REQBUFS, &request) == 0 && request.count == 2,
              "REQBUFS did not return the two-frame policy");
        std::cout << "allocation requested=" << requested << " returned=" << request.count << std::endl;
    }
}
void Next(himax::Capture& capture, hx_raw_frame& frame) {
    const auto deadline = Now() + 5000000000ULL;
    while (Now() < deadline) {
        hx_raw_state state{};
        while (capture.Event(state)) {}
        Check(capture.Error().empty(), capture.Error());
        if (capture.Frame(frame)) return;
        Check(capture.Error().empty(), capture.Error());
        pollfd pending{capture.Fd(), POLLIN | POLLPRI, 0};
        const auto result = poll(&pending, 1, 10);
        Check(result >= 0 || errno == EINTR, "Queue poll failed");
        Check(!(pending.revents & (POLLERR | POLLHUP | POLLNVAL)), "Queue disconnected");
    }
    throw std::runtime_error("No next queue packet before bounded deadline");
}
void Run(const char* path, const char* recording) {
    std::ofstream output(recording, std::ios::binary | std::ios::noreplace);
    Check(output.good(), "Cannot exclusively create queue evidence");
    Allocation(path);
    himax::Capture capture;
    Check(capture.Open(path) && capture.Start(), capture.Error());
    // Wait until the first completion without dequeuing/requeuing anything.
    const auto deadline = Now() + 5000000000ULL;
    for (;;) {
        hx_raw_state state{};
        while (capture.Event(state)) {}
        Check(capture.Error().empty(), capture.Error());
        pollfd pending{capture.Fd(), POLLIN | POLLPRI, 0};
        const auto result = poll(&pending, 1, 10);
        Check(result >= 0 || errno == EINTR, "Initial queue poll failed");
        Check(!(pending.revents & (POLLERR | POLLHUP | POLLNVAL)), "Initial queue disconnected");
        if (result > 0 && (pending.revents & POLLIN)) break;
        Check(Now() < deadline, "No initial completion");
    }
    Check(capture.RefreshInfo(), capture.Error());
    Check(himax::WriteHxstHeader(output, capture.Info().project_id,
                                capture.Info().coordinate_flags), "Cannot write queue header");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    hx_raw_frame first{}, second{}, resumed{};
    Next(capture, first);
    Next(capture, second);
    Check(first.epoch == second.epoch && second.sequence == first.sequence + 1,
          "Full queue did not retain oldest consecutive packets");
    Check(Now() >= first.timestamp_ns + 100000000ULL,
          "Oldest packet was overwritten/relabeled while consumer was paused");
    Check(second.timestamp_ns > first.timestamp_ns && second.timestamp_ns - first.timestamp_ns < 50000000,
          "Retained packets do not describe the original consecutive producer times");
    Next(capture, resumed);
    Check(resumed.epoch == first.epoch && resumed.sequence > second.sequence + 1 &&
          Now() >= resumed.timestamp_ns && Now() - resumed.timestamp_ns < 50000000,
          "Resumed producer did not preserve dropped-new sequence gap/freshness in unchanged epoch");
    for (const auto* frame : {&first, &second, &resumed})
        output.write(reinterpret_cast<const char*>(frame), sizeof(*frame));
    std::array<uint8_t, Frame::kMasterFrameSize> thp{};
    Check(himax::ToThpMaster({resumed.payload, resumed.payload_size}, thp) == himax::FrameError::None,
          "Resumed SPI data is not a valid master");
    Frame::MasterSuffixView suffix;
    suffix.LoadFromBytes(thp.data() + Frame::kMasterSuffixOffset);
    Check(!suffix.hasFinger() && !suffix.fwResetFlag(), "Finger/SOS during hands-off queue test");
    output.close();
    Check(!output.fail(), "Cannot close queue evidence");
    std::cout << "retained_epoch=" << first.epoch << " oldest_sequence=" << first.sequence
              << " second_sequence=" << second.sequence << " resumed_sequence=" << resumed.sequence
              << " oldest_age_ms=" << (Now() - first.timestamp_ns) / 1000000.0
              << " dropped_between=" << resumed.sequence - second.sequence - 1
              << " resumed_valid=1 (deliberate stale frames, not solver/session acceptance)" << std::endl;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: himax-queue-hardware-test /dev/himax-raw NEW.hxst\n"
                  << "Stop touch service; keep hands off. Pauses consumer for 120ms, never IRQ/SPI.\n";
        return 2;
    }
    try { Run(argv[1], argv[2]); return 0; }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
