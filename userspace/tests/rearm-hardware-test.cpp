// SPDX-License-Identifier: MIT
// Opt-in only: hands-off real-device validation, not registered with CTest.
// Uses legitimate firmware IDLE quiet to exercise rearm, not IRQ fault injection.
#include "recording.h"
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
void Check(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

class Test {
public:
    Test(const char* device, const char* path)
        : output(path, std::ios::binary | std::ios::noreplace) {
        Check(output.good(), "Cannot exclusively create recording");
        Check(capture.Open(device) && capture.Start(), capture.Error());
        Check(himax::WriteHxstHeader(output, capture.Info().project_id,
                                    capture.Info().coordinate_flags), "Cannot write header");
    }

    void Run(const char* device, bool transportOnly) {
        for (unsigned i = 0; i < 24; ++i) Next(Now() + 5000000000ULL);
        const uint64_t epoch = packet.epoch;
        auto request = Request();
        --request.size;
        Expect(capture.Fd(), request, EINVAL, "bad-size");
        request = Request();
        ++request.version;
        Expect(capture.Fd(), request, EINVAL, "bad-version");
        for (unsigned i : {0u, 1u}) {
            request = Request();
            request.reserved[i] = 1;
            Expect(capture.Fd(), request, EINVAL, "reserved");
        }
        request = Request();
        request.epoch = 0;
        Expect(capture.Fd(), request, ESTALE, "bad-epoch");
        request = Request();
        request.sequence = UINT64_MAX;
        Expect(capture.Fd(), request, ESTALE, "future-sequence");
        himax::Capture other;
        Check(other.Open(device, false), other.Error());
        Expect(other.Fd(), Request(), EBUSY, "other-owner");

        // This is a supported command already verified on this panel. Do not
        // disable IRQs or perturb parent-controller state to force a timeout.
        for (unsigned i = 0; ; ++i) {
            Next(Now() + 2000000000ULL);
            if (capture.Command(packet.epoch, packet.sequence, HX_RAW_ENTER_IDLE, 0)) break;
            Check(errno == EAGAIN && i < 3, "Cannot enter firmware IDLE");
        }
        const auto idleDeadline = Now() + 5000000000ULL;
        do { Next(idleDeadline); } while (suffix.idleFlag() != 1);
        const auto quietDeadline = Now() + 5000000000ULL;
        for (;;) {
            while (Read()) Check(Now() < quietDeadline, "No IDLE quiet before deadline");
            Check(Now() < quietDeadline, "No 250ms IDLE quiet interval; trial inconclusive");
            if (Now() - stream.ProgressNs() >= 250000000ULL) break;
            Poll(10);
        }
        const auto prior = packet;
        const auto before = Now();
        const bool rearmed = capture.Rearm(prior.epoch, prior.sequence);
        const int error = rearmed ? 0 : errno;
        Check(rearmed, "IDLE rearm ioctl: " + std::string(std::strerror(error)));
        const auto after = Now();
        std::cout << "rearm-transport=OK epoch=" << prior.epoch << " sequence=" << prior.sequence
                  << " quiet_ms=" << (before - stream.ProgressNs()) / 1000000ULL << std::endl;
        Next(Now() + 1000000000ULL, transportOnly);
        Check(packet.epoch == epoch && packet.sequence == prior.sequence + 1,
              "Kick did not deliver exactly one packet in unchanged epoch");
        Check(packet.timestamp_ns >= before && packet.timestamp_ns <= after,
              "Packet timestamp is not inside the synchronous rearm transaction");
        const bool same = packet.payload_size == prior.payload_size &&
            std::memcmp(packet.payload, prior.payload, packet.payload_size) == 0;
        std::cout << "rearm-valid=" << (packetValid ? "YES" : "NO")
                  << " epoch=" << packet.epoch << " sequence=" << packet.sequence
                  << " payload_identical_to_previous=" << same << std::endl;
        // Byte inequality does not establish a fresh firmware scan either.
        std::cout << "firmware-scan-freshness=UNPROVEN (no verified hardware frame counter)\n";
        request = Request();
        request.sequence = prior.sequence;
        Expect(capture.Fd(), request, ESTALE, "old-token-after-kick");
        request = Request();
        const int recent = Error(capture.Fd(), request);
        Check(recent == EAGAIN || recent == ESTALE, "Immediate kick was not refused");
        std::cout << "immediate-rearm-rejected errno=" << recent << std::endl;
        // No more kicks: verify that the normal IRQ path continues independently.
        Next(Now() + 5000000000ULL);
        Check(packet.epoch == epoch, "Normal IRQ flow required an AFE restart");
        std::cout << "normal-IRQ-after-rearm epoch=" << packet.epoch << " sequence=" << packet.sequence
                  << " idle=" << suffix.idleFlag() << std::endl;
        request = Request();
        int type = V4L2_BUF_TYPE_META_CAPTURE;
        Check(ioctl(capture.Fd(), VIDIOC_STREAMOFF, &type) == 0, "STREAMOFF failed");
        Expect(capture.Fd(), request, ESTALE, "streamoff");
        output.close();
        Check(!output.fail(), "Cannot close recording");
        std::cout << "recorded_frames=" << frames << " invalid=" << invalid
                  << " stale=" << stale << " (raw-only, not strict solver session)\n";
        std::cout << "IDLE quiet transport/gates/IRQ-continuation checked; no lost-IRQ or touch-quality claim\n";
    }

private:
    static int Error(int fd, const hx_raw_rearm& request) {
        return ioctl(fd, VIDIOC_HIMAX_REARM, &request) == 0 ? 0 : errno;
    }
    static void Expect(int fd, const hx_raw_rearm& request, int error, const char* name) {
        const int actual = Error(fd, request);
        Check(actual == error, std::string(name) + ": expected=" + std::to_string(error) +
              " actual=" + std::to_string(actual));
        std::cout << "gate " << name << " errno=" << actual << std::endl;
    }
    hx_raw_rearm Request() const {
        return {sizeof(hx_raw_rearm), HX_RAW_ABI_VERSION, packet.epoch, packet.sequence, {0, 0}};
    }
    void Poll(int ms) {
        pollfd fd{capture.Fd(), POLLIN | POLLPRI, 0};
        const int result = poll(&fd, 1, ms);
        Check(result >= 0 || errno == EINTR, "poll failed");
        Check(!(fd.revents & (POLLERR | POLLHUP | POLLNVAL)), "Raw stream disconnected");
    }
    bool Read(bool permitKickEmpty = false) {
        hx_raw_state state{};
        while (capture.Event(state)) stream.State(state, Now());
        Check(capture.Error().empty(), capture.Error());
        hx_raw_frame next{};
        if (!capture.Frame(next)) {
            Check(capture.Error().empty(), capture.Error());
            return false;
        }
        output.write(reinterpret_cast<const char*>(&next), sizeof(next));
        Check(output.good(), "Cannot save raw evidence");
        ++frames;
        Check(!packet.epoch || packet.epoch == next.epoch, "Unexpected AFE epoch transition");
        if (!stream.Accept(next, Now())) { ++stale; return true; }
        const auto error = himax::ToThpMaster({next.payload, next.payload_size}, thp);
        if (error != himax::FrameError::None) {
            ++invalid;
            std::cout << "invalid epoch=" << next.epoch << " sequence=" << next.sequence
                      << " reason=" << himax::FrameErrorName(error) << std::endl;
            if (validSeen) {
                Check(permitKickEmpty && error == himax::FrameError::AllZero,
                      "Non-startup invalid frame; inspect recording");
                packet = next;
                packetValid = false;
            } else Check(invalid < 5, "Too many invalid startup frames");
            return true;
        }
        suffix.LoadFromBytes(thp.data() + Frame::kMasterSuffixOffset);
        Check(!suffix.hasFinger(), "Finger detected; hands-off trial aborted");
        Check(!suffix.fwResetFlag(), "Firmware SOS; inspect recording");
        packet = next;
        packetValid = validSeen = true;
        return true;
    }
    void Next(uint64_t deadline, bool permitKickEmpty = false) {
        const auto previous = packet.sequence;
        while (Now() < deadline) {
            if (Read(permitKickEmpty) && packet.sequence != previous) return;
            Poll(10);
        }
        throw std::runtime_error("No new valid master packet before trial deadline");
    }

    std::ofstream output;
    himax::Capture capture;
    himax::StreamState stream;
    hx_raw_frame packet{};
    Frame::MasterSuffixView suffix{};
    std::array<uint8_t, Frame::kMasterFrameSize> thp{};
    uint64_t frames = 0, invalid = 0, stale = 0;
    bool packetValid = false, validSeen = false;
};
} // namespace

int main(int argc, char** argv) {
    const bool transportOnly = argc == 4 && std::strcmp(argv[3], "--transport-only") == 0;
    if (argc != 3 && !transportOnly) {
        std::cerr << "Usage: himax-rearm-hardware-test /dev/himax-raw NEW.hxst [--transport-only]\n"
                  << "Stop the touch service and keep hands off. This starts a new AFE stream,\n"
                  << "enters firmware IDLE, and performs one quiet-period no-reset master read.\n"
                  << "Default requires a valid kicked frame. --transport-only permits and records an\n"
                  << "all-zero kicked frame, explicitly without a validity/recovery claim.\n"
                  << "Does not disable IRQ/controller or simulate a lost IRQ. Restore service afterward.\n";
        return 2;
    }
    try { Test test(argv[1], argv[2]); test.Run(argv[1], transportOnly); return 0; }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
