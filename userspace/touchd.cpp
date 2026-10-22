// SPDX-License-Identifier: MIT
#include "native-frame.h"
#include "TsaCoreSolver.h"
#include "input.h"
#include "v4l2-capture.h"
#include "stream-state.h"
#include "service-notify.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <poll.h>
#include <time.h>

namespace {
volatile std::sig_atomic_t stopped = 0;
volatile std::sig_atomic_t dumpRequested = 0;
void Stop(int) { stopped = 1; }
void Dump(int) { dumpRequested = 1; }
uint64_t NowNs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000000000 + now.tv_nsec;
}
}  // namespace

// Keep the public transport command ABI independent of upstream enum ordering.
static hx_raw_action HardwareAction(Solvers::Tsa::AfeActionType action) {
    using Action = Solvers::Tsa::AfeActionType;
    switch (action) {
    case Action::ClearStatus: return HX_RAW_CLEAR_STATUS;
    case Action::ForceToFreqPoint: return HX_RAW_FORCE_FREQ;
    case Action::ForceToScanRate: return HX_RAW_FORCE_RATE;
    case Action::EnterIdle: return HX_RAW_ENTER_IDLE;
    case Action::StartCalibration: return HX_RAW_CALIBRATE;
    }
    return HX_RAW_RESET;  // Unknown actions are rejected by the caller below.
}

int main(int argc, char** argv) {
    std::string device, project, recording;
    bool observe = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--observe") observe = true;
        else if (arg == "--device" && i + 1 < argc) device = argv[++i];
        else if (arg == "--project" && i + 1 < argc) project = argv[++i];
        else if (arg == "--record" && i + 1 < argc) recording = argv[++i];
        else {
            std::cerr << "Usage: himax-touchd [--device /dev/videoN] [--project ID]\n"
                      << "                    [--observe [--record FILE.hxst]]\n";
            return arg == "--help" ? 0 : 2;
        }
    }
    if (!project.empty() && Solvers::Tsa::ResolvePanelProjectId(project) != project) {
        std::cerr << "Unknown project ID\n";
        return 2;
    }
    if (!recording.empty() && !observe) {
        std::cerr << "Recording currently requires --observe\n";
        return 2;
    }
    struct sigaction action {};
    action.sa_handler = Stop;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    action.sa_handler = Dump;
    sigaction(SIGUSR1, &action, nullptr);

    himax::Capture capture;
    himax::Input input;
    if (!capture.Open(device) || !capture.Start()) {
        std::cerr << capture.Error() << '\n';
        return 1;
    }
    if (project.empty()) {
        project = Solvers::Tsa::ResolvePanelProjectId(capture.Info().project_id);
        if (project != capture.Info().project_id)
            std::cerr << "OTP unavailable; using upstream fallback " << project << '\n';
    }
    if (!observe && !input.Open()) {
        std::cerr << input.Error() << '\n';
        return 1;
    }
    std::ofstream output;
    if (!recording.empty()) {
        output.open(recording, std::ios::binary | std::ios::trunc);
        if (!output) {
            std::cerr << "Cannot open recording\n";
            return 1;
        }
    }
    auto solver = std::make_unique<Solvers::Tsa::TsaCoreSolver>();
    if (!solver->SetPanelProjectId(project)) return 1;
    auto frame = std::make_unique<Solvers::HeatmapFrame>();
    auto packet = std::make_unique<hx_raw_frame>();
    std::array<uint8_t, Frame::kMasterFrameSize> thp{};
    std::array<himax::Contact, 10> contacts{};
    uint64_t epoch = 0, lastNs = NowNs();
    uint64_t frames = 0, stale = 0, bad = 0;
    himax::StreamState stream;
    bool afeStarted = false;
    unsigned int consecutiveBad = 0;
    uint64_t solved = 0, solveTotal = 0, solveMax = 0, deliveryMax = 0;
    std::array<uint64_t, 201> deliveryUs{};
    std::array<uint64_t, 5> actionRequests{}, actionSuccess{};
    uint64_t idleFrames = 0, calibrationFrames = 0, frequencyFrames = 0;
    uint64_t resetRequests = 0, resetSuccess = 0;
    int exitCode = 0;
    uint64_t heartbeat = NowNs();
    const auto printStats = [&] {
        std::cerr << "frames=" << frames << " bad=" << bad << " stale=" << stale << '\n';
        if (solved)
            std::cerr << "solver_us mean=" << solveTotal / solved / 1000
                      << " max=" << solveMax / 1000 << '\n';
        if (!observe) {
            uint64_t reports = 0;
            for (auto count : deliveryUs) reports += count;
            std::cerr << "touch_reports=" << reports << '\n';
            for (unsigned int percentile : {95u, 99u}) {
                uint64_t cumulative = 0;
                for (size_t i = 0; reports && i < deliveryUs.size(); ++i) {
                    cumulative += deliveryUs[i];
                    if (cumulative * 100 >= reports * percentile) {
                        std::cerr << "irq_to_uinput_us p" << percentile
                                  << (i + 1 == deliveryUs.size() ? ">=" : "<")
                                  << (i + (i + 1 != deliveryUs.size())) * 50 << '\n';
                        break;
                    }
                }
            }
            if (reports) std::cerr << "irq_to_uinput_us max=" << deliveryMax / 1000 << '\n';
        }
        std::cerr << "afe_frames idle=" << idleFrames << " calibration_done=" << calibrationFrames
                  << " frequency_done=" << frequencyFrames << '\n';
        constexpr const char* names[] = {"clear-status", "frequency", "rate", "idle", "calibrate"};
        for (size_t i = 0; i < actionRequests.size(); ++i)
            if (actionRequests[i])
                std::cerr << "afe_action " << names[i] << " requested=" << actionRequests[i]
                          << " succeeded=" << actionSuccess[i]
                          << " failed=" << actionRequests[i] - actionSuccess[i] << '\n';
        if (resetRequests)
            std::cerr << "afe_reset requested=" << resetRequests << " succeeded=" << resetSuccess
                      << " failed=" << resetRequests - resetSuccess << '\n';
    };
    himax::NotifyService("READY=1");
    std::cerr << (observe ? "Observing " : "Solving ") << capture.Path() << " project=" << project << '\n';
    while (!stopped) {
        if (dumpRequested) { dumpRequested = 0; printStats(); }
        if (stream.TimedOut(NowNs())) {
            std::cerr << "AFE producer stalled for four seconds; restarting through supervisor\n";
            exitCode = 1;
            break;
        }
        if (NowNs() - heartbeat >= 1000000000) {
            himax::NotifyService("WATCHDOG=1");
            heartbeat = NowNs();
        }
        pollfd pending{capture.Fd(), POLLIN | POLLPRI, 0};
        const int ret = poll(&pending, 1, 50);
        if (ret < 0) {
            if (errno == EINTR) continue;
            std::cerr << "poll: " << std::strerror(errno) << '\n';
            exitCode = 1;
            break;
        }
        if (!observe && input.Active() && NowNs() - lastNs > 250000000) {
            if (!input.Release()) { exitCode = 1; break; }
            std::cerr << "No fresh frames for 250 ms; released contacts\n";
        }
        if (!ret) continue;
        if (pending.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            std::cerr << "V4L2 stream disconnected\n";
            exitCode = 1;
            break;
        }
        if (pending.revents & POLLPRI) {
            hx_raw_state state{};
            while (capture.Event(state)) {
                if (stream.State(state, NowNs()) && (!state.running || state.epoch != epoch)) {
                    if (!observe && !input.Release()) return 1;
                    solver->OnAfeStopped();
                    afeStarted = false;
                }
            }
        }
        if (!capture.Error().empty()) {
            std::cerr << capture.Error() << '\n';
            exitCode = 1;
            break;
        }
        if (!(pending.revents & POLLIN) || !capture.Frame(*packet)) {
            if (!capture.Error().empty()) {
                std::cerr << capture.Error() << '\n';
                exitCode = 1;
                break;
            }
            continue;
        }
        ++frames;
        const uint64_t now = NowNs();
        if (!stream.Accept(*packet, now)) {
            ++stale;
            continue;
        }
        if (packet->epoch != epoch || !afeStarted) {
            if (!observe && !input.Release()) return 1;
            solver->OnAfeStopped();
            if (packet->flags & HX_RAW_FRAME_SOS) solver->OnAfeSos();
            solver->OnAfeStarted();
            epoch = packet->epoch;
            afeStarted = true;
            std::cerr << "AFE epoch=" << epoch << '\n';
        }
        solver->SetChargerPlugged(packet->flags & HX_RAW_FRAME_CHARGER);
        const auto error = himax::ToThpMaster(packet->payload, thp);
        bool requestReset = false;
        if (error != himax::FrameError::None) {
            ++bad;
            requestReset = ++consecutiveBad >= 5;
        } else {
            consecutiveBad = 0;
            lastNs = now;
            frame->rawPtr = thp.data();
            frame->rawLen = thp.size();
            const uint64_t solveStart = NowNs();
            const bool report = solver->Process(*frame, packet->timestamp_ns / 1000000);
            const uint64_t solveTime = NowNs() - solveStart;
            ++solved;
            solveTotal += solveTime;
            solveMax = std::max(solveMax, solveTime);
            idleFrames += !!(solver->LastAfeStatus() & Solvers::Tsa::AfeStatus::kIdle);
            calibrationFrames += !!(solver->LastAfeStatus() & Solvers::Tsa::AfeStatus::kCalibDone);
            frequencyFrames += !!(solver->LastAfeStatus() & Solvers::Tsa::AfeStatus::kFreqShiftDone);
            if (!solver->IsAvailable()) {
                std::cerr << "TSACore initialization failed\n";
                return 1;
            }
            requestReset = solver->LastAfeStatus() & Solvers::Tsa::AfeStatus::kSos;
            if (!requestReset && !observe && report) {
                size_t count = 0;
                for (const auto& t : frame->touch.output.contacts) {
                    if (!t.isReported || t.state == Solvers::TouchStateUp) continue;
                    if (count >= contacts.size() || t.id < 1 || t.id > 32 ||
                        !std::isfinite(t.x) || !std::isfinite(t.y)) {
                        std::cerr << "Invalid TSACore contact\n";
                        return 1;
                    }
                    contacts[count++] = {
                        static_cast<uint8_t>(t.id - 1),
                        static_cast<uint16_t>(std::lround(std::clamp(t.x * 10, 0.0f, 25599.0f))),
                        static_cast<uint16_t>(std::lround(std::clamp(t.y * 10, 0.0f, 15999.0f))),
                        static_cast<uint16_t>(std::clamp(t.signalSum, 0, 4095)),
                    };
                }
                if (!input.Report(std::span(contacts.data(), count))) {
                    std::cerr << input.Error() << '\n';
                    return 1;
                }
                const uint64_t delivery = NowNs() - packet->timestamp_ns;
                deliveryMax = std::max(deliveryMax, delivery);
                ++deliveryUs[std::min<uint64_t>(delivery / 50000, deliveryUs.size() - 1)];
            }
            if (!requestReset) {
                for (const auto& request : solver->PendingAfeActions()) {
                    const auto actionType = HardwareAction(request.type);
                    if (actionType == HX_RAW_RESET) {
                        std::cerr << "Unknown upstream AFE action\n";
                        return 1;
                    }
                    const bool ok = !observe && capture.Command(epoch, packet->sequence, actionType, request.param);
                    if (!observe) {
                        ++actionRequests[actionType];
                        actionSuccess[actionType] += ok;
                    }
                    solver->OnAfeActionExecuted(request, ok);
                    if (!observe && !ok && errno != ESTALE && errno != EAGAIN)
                        std::cerr << "AFE action failed: " << std::strerror(errno) << '\n';
                }
            }
        }
        if (requestReset && !observe) {
            if (!input.Release()) return 1;
            ++resetRequests;
            if (capture.Command(epoch, packet->sequence, HX_RAW_RESET, 0)) {
                ++resetSuccess;
            } else if (errno != ESTALE && errno != EAGAIN) {
                std::cerr << "AFE reset request failed: " << std::strerror(errno) << '\n';
                exitCode = 1;
                break;
            }
        }
        if (output.is_open()) {
            output.write(reinterpret_cast<const char*>(packet.get()), sizeof(*packet));
            if (!output) {
                std::cerr << "Recording write failed\n";
                return 1;
            }
        }
    }
    if (!observe) input.Release();
    printStats();
    return exitCode;
}
