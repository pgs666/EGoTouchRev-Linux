// SPDX-License-Identifier: MIT
// Opt-in real-device test. Never registered with CTest; keep hands off panel.
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

void Check(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

class Test {
public:
    Test(const char* device, const char* recording) : output(recording, std::ios::binary) {
        Check(output.good(), "Cannot create test recording");
        Check(capture.Open(device) && capture.Start(), capture.Error());
        Check(capture.RefreshInfo(), capture.Error());
        Check(himax::WriteHxstHeader(output, capture.Info().project_id,
                                    capture.Info().coordinate_flags), "Cannot write HXRC header");
    }

    template <typename Predicate>
    void Wait(const char* name, Predicate predicate, uint64_t after = 0) {
        const uint64_t start = Now(), deadline = start + 5000000000ULL;
        try {
            while (Now() < deadline) {
                Next(deadline);
                Check(!suffix.hasFinger(), "Finger detected; aborting hands-off AFE test");
                if (packet.timestamp_ns < after || !predicate(suffix)) continue;
                std::cout << name << " elapsed_ms=" << (Now() - start) / 1000000ULL;
                PrintSuffix();
                return;
            }
        } catch (const std::exception& error) {
            std::cerr << "Last suffix while waiting for " << name << ':' << std::endl;
            PrintSuffix();
            throw std::runtime_error(std::string(name) + ": " + error.what());
        }
        throw std::runtime_error(std::string(name) + " did not reach expected suffix state");
    }

    uint64_t Send(hx_raw_action action, uint32_t parameter) {
        const uint64_t deadline = Now() + 2000000000ULL;
        do {
            // Always drain a new frame before commanding, including retries.
            Next(deadline);
            Check(!suffix.hasFinger(), "Finger detected before AFE command; aborting");
            if (capture.Command(packet.epoch, packet.sequence, action, parameter)) {
                const uint64_t sent = Now();
                std::cout << "command action=" << action << " param=" << parameter
                          << " transport=OK epoch=" << packet.epoch
                          << " sequence=" << packet.sequence << std::endl;
                return sent;
            }
            const int error = errno;
            Check(error == EAGAIN, "AFE command failed: " + std::string(std::strerror(error)));
        } while (Now() < deadline);
        throw std::runtime_error("AFE command remained rate-limited for two seconds");
    }

    void Finish() {
        output.flush();
        Check(output.good(), "Cannot flush test recording");
        output.close();
        Check(!output.fail(), "Cannot close test recording");
        std::cout << "recorded_frames=" << frames << " startup_invalid=" << startupInvalid
                  << " (raw recording, not solver-equivalent session)" << std::endl;
    }

    Frame::MasterSuffixView suffix{};

private:
    void PrintSuffix() const {
        std::cout << " epoch=" << packet.epoch << " sequence=" << packet.sequence
                  << " idle=" << suffix.idleFlag() << " diag=" << suffix.diagStatus()
                  << " freq_slot=" << suffix.words[10]
                  << " freq_done=" << suffix.freqShiftDone() << std::endl;
    }

    void Next(uint64_t deadline) {
        while (Now() < deadline) {
            hx_raw_state state{};
            while (capture.Event(state)) stream.State(state, Now());
            Check(capture.Error().empty(), capture.Error());
            if (capture.Frame(packet) && stream.Accept(packet, Now())) {
                Check(!epoch || packet.epoch == epoch, "Unexpected AFE restart during command test");
                epoch = packet.epoch;
                output.write(reinterpret_cast<const char*>(&packet), sizeof(packet));
                Check(output.good(), "Cannot save raw AFE evidence");
                ++frames;
                const auto error = himax::ToThpMaster(packet.payload, thp);
                if (error != himax::FrameError::None) {
                    Check(!validSeen && ++startupInvalid <= 4,
                          "Invalid master: " + std::string(himax::FrameErrorName(error)));
                    continue;
                }
                validSeen = true;
                suffix.LoadFromBytes(thp.data() + Frame::kMasterSuffixOffset);
                Check(!suffix.fwResetFlag(), "Firmware reported SOS during command test");
                return;
            }
            Check(capture.Error().empty(), capture.Error());
            pollfd fd{capture.Fd(), POLLIN | POLLPRI, 0};
            const int result = poll(&fd, 1, 50);
            Check(result >= 0 || errno == EINTR, "poll failed");
            Check(!(fd.revents & (POLLERR | POLLHUP | POLLNVAL)), "Raw transport disconnected");
        }
        throw std::runtime_error("No qualifying frame before AFE test deadline");
    }

    std::ofstream output;
    himax::Capture capture;
    himax::StreamState stream;
    hx_raw_frame packet{};
    std::array<uint8_t, Frame::kMasterFrameSize> thp{};
    uint64_t epoch = 0, frames = 0;
    unsigned int startupInvalid = 0;
    bool validSeen = false;
};
}  // namespace

int main(int argc, char** argv) {
    const bool basic = argc == 4 && std::strcmp(argv[3], "--basic") == 0;
    const bool frequencyOnly = argc == 4 && std::strcmp(argv[3], "--frequency-only") == 0;
    if (argc != 3 && !basic && !frequencyOnly) {
        std::cerr << "Usage: himax-afe-hardware-test /dev/himax-raw OUTPUT.hxst\n"
                  << "       [--basic | --frequency-only]\n"
                  << "Stop touch service and keep hands off; test recalibrates, switches\n"
                  << "both supported frequencies and enters IDLE. --basic skips frequency\n"
                  << "commands (not frequency acceptance). --frequency-only skips explicit\n"
                  << "recalibration and IDLE; waits 240 clean frames before switching.\n"
                  << "Restart service afterward.\n";
        return 2;
    }
    try {
        Test test(argv[1], argv[2]);
        // Startup calibration must first latch a marker. Observe a clean,
        // hands-off interval before deliberately starting another calibration.
        test.Wait("startup-calibration-done", [](const auto& s) { return s.diagStatus() == 0xbb; });
        unsigned int clean = 0;
        test.Wait("hands-off-24-frames", [&](const auto&) { return ++clean == 24; });
        auto sent = test.Send(HX_RAW_CLEAR_STATUS, 8);
        test.Wait("calibration-marker-cleared", [](const auto& s) { return s.diagStatus() != 0xbb; }, sent);
        if (!frequencyOnly) {
            sent = test.Send(HX_RAW_CALIBRATE, 0);
            test.Wait("explicit-calibration-done", [](const auto& s) { return s.diagStatus() == 0xbb; }, sent);
            sent = test.Send(HX_RAW_CLEAR_STATUS, 8);
            test.Wait("explicit-calibration-ack", [](const auto& s) { return s.diagStatus() != 0xbb; }, sent);
        } else {
            std::cout << "explicit-calibration: SKIPPED (--frequency-only)" << std::endl;
            clean = 0;
            test.Wait("settled-240-frames", [&](const auto& s) {
                Check(s.diagStatus() != 0xbb, "Calibration marker unexpectedly relatched while settling");
                return ++clean == 240;
            });
        }

        if (!basic) {
            const unsigned int original = test.suffix.words[10] != 0;
            sent = test.Send(HX_RAW_CLEAR_STATUS, 4);
            test.Wait("frequency-marker-cleared", [](const auto& s) { return !s.freqShiftDone(); }, sent);
            for (const unsigned int target : {1u - original, original}) {
                sent = test.Send(HX_RAW_FORCE_FREQ, target);
                test.Wait("frequency-switch-done", [&](const auto& s) {
                    return (s.words[10] != 0) == (target != 0) && s.freqShiftDone();
                }, sent);
                sent = test.Send(HX_RAW_CLEAR_STATUS, 4);
                test.Wait("frequency-switch-ack", [](const auto& s) { return !s.freqShiftDone(); }, sent);
            }
        } else {
            std::cout << "frequency: SKIPPED (--basic); no frequency acceptance claim" << std::endl;
        }
        if (!frequencyOnly) {
            test.Send(HX_RAW_FORCE_RATE, 0);
            std::cout << "rate0: transport verified only; adapter reports a fixed 120 Hz,\n"
                      << "so this is not independent scan-rate completion proof." << std::endl;
            sent = test.Send(HX_RAW_ENTER_IDLE, 0);
            test.Wait("idle-entered", [](const auto& s) { return s.idleFlag() == 1; }, sent);
        } else {
            std::cout << "rate/IDLE: SKIPPED (--frequency-only)" << std::endl;
        }
        test.Finish();
        std::cout << "Selected AFE suffix handshakes passed: mode="
                  << (basic ? "basic" : frequencyOnly ? "frequency-only" : "full") << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
