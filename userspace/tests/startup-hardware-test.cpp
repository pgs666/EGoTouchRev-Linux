// SPDX-License-Identifier: MIT
// Opt-in only. Exercises the client lifecycle, never the parent controller.
// Not registered with CTest: stop the solver and keep hands off the screen.
#include "recording.h"
#include "stream-state.h"
#include "v4l2-capture.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <stdexcept>

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
        Check(output.good(), "Cannot exclusively create raw evidence");
        start = Now();
        Check(capture.Open(device) && capture.Start(), capture.Error());
        std::cout << "STREAMON_return_ms=" << (Now() - start) / 1000000.0 << std::endl;
    }

    void Run() {
        uint64_t priorEpoch = 0;
        for (unsigned cycle = 0; cycle < 4; ++cycle) {
            const auto requested = cycle ? SendReset() : start;
            const auto deadline = Now() + 5000000000ULL;
            unsigned clean = 0;
            uint64_t cycleEpoch = 0;
            bool calibration = false;
            while (clean < 24) {
                Next(deadline);
                // Buffers queued before RESET can still be delivered. Record
                // them, but they cannot prove a new initialized epoch.
                if (packet.epoch == priorEpoch) continue;
                Check(!priorEpoch || packet.epoch > priorEpoch, "Reset epoch did not advance");
                if (!cycleEpoch) cycleEpoch = packet.epoch;
                Check(packet.epoch == cycleEpoch, "Unexpected extra epoch during startup/reset acceptance");
                Frame::MasterSuffixView suffix;
                suffix.LoadFromBytes(thp.data() + Frame::kMasterSuffixOffset);
                Check(!suffix.hasFinger(), "Finger detected; aborting hands-off test");
                Check(!suffix.fwResetFlag(), "Firmware reports SOS");
                calibration |= suffix.diagStatus() == 0xbb;
                if (!clean)
                    std::cout << "first_valid cycle=" << cycle << " epoch=" << packet.epoch
                              << " sequence=" << packet.sequence
                              << " after_request_ms=" << (Now() - requested) / 1000000.0
                              << std::endl;
                ++clean;
            }
            Check(calibration, "No startup calibration-done suffix in 24 valid frames");
            priorEpoch = packet.epoch;
            std::cout << "cycle=" << cycle << " valid_frames=" << clean
                      << " calibration_done_seen=1 epoch=" << priorEpoch << std::endl;
        }
        output.close();
        Check(!output.fail(), "Cannot close raw evidence");
        std::cout << "recorded_frames=" << frames << " startup_invalid=" << invalid
                  << " stale=" << stale << " resets=3 (transport/suffix, not solver acceptance)"
                  << std::endl;
    }

private:
    void Next(uint64_t deadline) {
        while (Now() < deadline) {
            hx_raw_state state{};
            while (capture.Event(state)) {
                Check(stream.State(state, Now()), "Regressing lifecycle event");
                std::cout << "state running=" << state.running << " epoch=" << state.epoch
                          << " observed_ns=" << Now() << std::endl;
            }
            Check(capture.Error().empty(), capture.Error());
            if (capture.Frame(packet)) {
                if (!header) {
                    Check(capture.RefreshInfo(), capture.Error());
                    Check(himax::WriteHxstHeader(output, capture.Info().project_id,
                                                capture.Info().coordinate_flags), "Cannot write header");
                    header = true;
                }
                output.write(reinterpret_cast<const char*>(&packet), sizeof(packet));
                Check(output.good(), "Cannot save raw packet");
                ++frames;
                if (!stream.Accept(packet, Now())) { ++stale; continue; }
                const auto error = himax::ToThpMaster({packet.payload, packet.payload_size}, thp);
                if (packet.epoch != epoch) { epoch = packet.epoch; validSeen = false; epochInvalid = 0; }
                if (error != himax::FrameError::None) {
                    ++invalid;
                    Check(!validSeen && ++epochInvalid < himax::MasterFrameHealth::kBadFrameLimit &&
                          error != himax::FrameError::BusFiller,
                          "Invalid packet: " + std::string(himax::FrameErrorName(error)));
                    continue;
                }
                validSeen = true;
                return;
            }
            Check(capture.Error().empty(), capture.Error());
            pollfd pending{capture.Fd(), POLLIN | POLLPRI, 0};
            const auto result = poll(&pending, 1, 20);
            Check(result >= 0 || errno == EINTR, "poll failed");
            Check(!(pending.revents & (POLLERR | POLLHUP | POLLNVAL)), "Stream disconnected");
        }
        throw std::runtime_error("No valid post-start/reset data before five-second deadline");
    }

    uint64_t SendReset() {
        const auto deadline = Now() + 3000000000ULL;
        do {
            Next(deadline);
            Frame::MasterSuffixView suffix;
            suffix.LoadFromBytes(thp.data() + Frame::kMasterSuffixOffset);
            Check(!suffix.hasFinger(), "Finger detected before reset");
            const auto request = Now();
            if (capture.Command(packet.epoch, packet.sequence, HX_RAW_RESET, 0)) {
                std::cout << "reset transport=OK epoch=" << packet.epoch
                          << " sequence=" << packet.sequence << std::endl;
                return request;
            }
            const auto error = errno;
            Check(error == EAGAIN || error == ESTALE,
                  "Reset transport: " + std::string(std::strerror(error)));
        } while (Now() < deadline);
        throw std::runtime_error("Reset remained stale/rate-limited");
    }

    std::ofstream output;
    himax::Capture capture;
    himax::StreamState stream;
    hx_raw_frame packet{};
    std::array<uint8_t, Frame::kMasterFrameSize> thp{};
    uint64_t start = 0, epoch = 0, frames = 0, invalid = 0, stale = 0;
    unsigned epochInvalid = 0;
    bool header = false, validSeen = false;
};
} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: himax-startup-hardware-test /dev/himax-raw NEW.hxst\n"
                  << "Stop touch service first; keep hands off. Performs three client AFE resets.\n";
        return 2;
    }
    try {
        Test test(argv[1], argv[2]);
        test.Run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
