// SPDX-License-Identifier: MIT
#include "native-frame.h"
#include "capture-session.h"
#include "afe-action.h"
#include "TsaCoreSolver.h"
#include "input.h"
#include "v4l2-capture.h"
#include "stream-state.h"
#include "service-notify.h"
#include "screen-rotation.h"
#include "solver-screen.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <poll.h>
#include <string_view>
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

int main(int argc, char** argv) {
    std::string device, project, recording, rotationFile;
    std::optional<int> fixedRotation;
    bool observe = false, infoOnly = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--observe") observe = true;
        else if (arg == "--info") infoOnly = true;
        else if (arg == "--device" && i + 1 < argc) device = argv[++i];
        else if (arg == "--project" && i + 1 < argc) project = argv[++i];
        else if (arg == "--record" && i + 1 < argc) recording = argv[++i];
        else if (arg == "--rotation" && i + 1 < argc) {
            fixedRotation = himax::ParseScreenRotation(argv[++i]);
            if (!fixedRotation) {
                std::cerr << "Invalid rotation; use 0..3 or 0/90/180/270\n";
                return 2;
            }
        } else if (arg == "--rotation-file" && i + 1 < argc) {
            rotationFile = argv[++i];
        } else {
            std::cerr << "Usage: himax-touchd [--device /dev/videoN] [--project ID]\n"
                      << "                    [--rotation 0..3|90|180|270] [--rotation-file FILE]\n"
                      << "                    [--observe] [--record FILE.hxst]\n"
                      << "       himax-touchd [--device /dev/videoN] --info\n";
            return arg == "--help" ? 0 : 2;
        }
    }
    if (!project.empty() && Solvers::Tsa::ResolvePanelProjectId(project) != project) {
        std::cerr << "Unknown project ID\n";
        return 2;
    }
    const bool autoProject = project.empty();
    if (infoOnly && (observe || !recording.empty() || !autoProject ||
                     fixedRotation || !rotationFile.empty())) {
        std::cerr << "--info cannot be combined with solver/capture options\n";
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
    if (infoOnly) {
        if (!capture.Open(device, false)) {
            std::cerr << capture.Error() << '\n';
            return 1;
        }
        const auto& info = capture.Info();
        std::cout << "device=" << capture.Path() << "\nabi=" << info.version
                  << "\nrows=" << info.rows << "\ncols=" << info.cols
                  << "\nproject_id=" << info.project_id
                  << "\ncoordinate_flags=" << info.coordinate_flags << '\n';
        return 0;
    }
    // Receive desktop state before STREAMON's synchronous hardware startup,
    // instead of creating the socket only after the first frame is available.
    himax::ScreenRotationProvider rotationProvider(rotationFile);
    rotationProvider.SetFixed(fixedRotation);
    rotationProvider.RequestInitial();
    if (!capture.Open(device) || !capture.Start()) {
        std::cerr << capture.Error() << '\n';
        return 1;
    }
    if (!observe && !input.Open(capture.Info().coordinate_flags)) {
        std::cerr << input.Error() << '\n';
        return 1;
    }
    std::unique_ptr<himax::SessionRecorder> recorder;
    std::unique_ptr<himax::SessionEvent> recordedFrame;
    if (!recording.empty()) {
        recorder = std::make_unique<himax::SessionRecorder>(recording);
        recordedFrame = std::make_unique<himax::SessionEvent>();
        if (recorder->Error()) {
            std::cerr << "Cannot create recording: " << std::strerror(recorder->Error()) << '\n';
            return 1;
        }
    }
    // Early error returns must release input before joining the optional disk
    // writer. A stalled filesystem must not leave the last contact held.
    struct ReleaseOnExit {
        himax::Input& input;
        bool observe;
        ~ReleaseOnExit() { if (!observe) input.Release(); }
    } releaseOnExit{input, observe};
    bool recordingFailed = false;
    const auto recordEvent = [&](const himax::SessionEvent& event) {
        if (recorder && !recorder->Submit(event) && !recordingFailed) {
            recordingFailed = true;
            std::cerr << "Recording incomplete; touch continues: "
                      << std::strerror(recorder->Error()) << '\n';
        }
    };
    auto solver = std::make_unique<Solvers::Tsa::TsaCoreSolver>();
    himax::SolverScreenState solverScreen;
    auto frame = std::make_unique<Solvers::HeatmapFrame>();
    auto packet = std::make_unique<hx_raw_frame>();
    std::array<uint8_t, Frame::kMasterFrameSize> thp{};
    std::array<himax::Contact, 10> contacts{};
    uint64_t epoch = 0, lastNs = NowNs();
    uint64_t frames = 0, stale = 0, bad = 0;
    himax::StreamState stream;
    bool afeStarted = false;
    bool profileSelected = false;
    bool recordingHeaderWritten = false;
    himax::MasterFrameHealth frameHealth;
    std::array<uint64_t, static_cast<size_t>(himax::FrameError::Count)> frameErrors{};
    uint64_t solved = 0, solveTotal = 0, solveMax = 0, deliveryMax = 0;
    std::array<uint64_t, 201> deliveryUs{};
    std::array<uint64_t, 5> actionRequests{}, actionSuccess{};
    uint64_t idleFrames = 0, calibrationFrames = 0, frequencyFrames = 0;
    uint64_t resetRequests = 0, resetSuccess = 0;
    uint64_t nextRotationPoll = 0;
    int exitCode = 0;
    uint64_t heartbeat = NowNs();
    const auto printStats = [&] {
        std::cerr << "frames=" << frames << " bad=" << bad << " stale=" << stale << '\n';
        for (size_t i = 1; i < frameErrors.size(); ++i)
            if (frameErrors[i])
                std::cerr << "frame_error "
                          << himax::FrameErrorName(static_cast<himax::FrameError>(i))
                          << '=' << frameErrors[i] << '\n';
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
        std::cerr << "screen_rotation=" << solverScreen.Rotation()
                  << " (-1 means no host orientation)\n";
        if (recorder)
            std::cerr << "recording_error=" << recorder->Error()
                      << " dropped=" << recorder->Dropped() << '\n';
    };
    const auto pollRotation = [&](uint64_t now) {
        if (now < nextRotationPoll) return;
        nextRotationPoll = now + 250000000ULL;
        const auto sample = rotationProvider.Poll();
        if (!sample || sample->value == solverScreen.Rotation()) return;
        solverScreen.SetRotation(sample->value);
        std::cerr << "screen_rotation=" << sample->value
                  << " source=" << sample->source << '\n';
    };
    himax::NotifyService("READY=1");
    std::cerr << (observe ? "Observing " : "Solving ") << capture.Path()
              << " project=" << (autoProject ? "pending-OTP" : project) << '\n';
    while (!stopped) {
        const uint64_t loopNow = NowNs();
        pollRotation(loopNow);
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
            if (recorder) {
                himax::SessionEvent event;
                event.kind = himax::SessionKind::Release;
                event.hostTimeNs = NowNs();
                recordEvent(event);
            }
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
                if (!stream.State(state, NowNs())) continue;
                if (recorder) {
                    himax::SessionEvent event;
                    event.kind = himax::SessionKind::State;
                    event.hostTimeNs = NowNs();
                    event.state = state;
                    recordEvent(event);
                }
                if (!state.running || state.epoch != epoch) {
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
            // STREAMON while the panel is off does not read OTP. Only a
            // running epoch proves that hardware initialization attempted it.
            if (!capture.RefreshInfo()) {
                std::cerr << capture.Error() << '\n';
                return 1;
            }
            if (autoProject) {
                project = Solvers::Tsa::ResolvePanelProjectId(capture.Info().project_id);
                if (project != capture.Info().project_id)
                    std::cerr << "OTP unavailable; using upstream fallback " << project << '\n';
            }
            if ((profileSelected && project != solver->PanelProjectId()) ||
                !solver->SetPanelProjectId(project)) {
                std::cerr << "Panel project changed within the capture session\n";
                return 1;
            }
            profileSelected = true;
            if (recorder && !recordingHeaderWritten) {
                himax::SessionEvent event;
                event.kind = himax::SessionKind::Begin;
                std::copy(project.begin(), project.end(), event.project.begin());
                event.coordinateFlags = capture.Info().coordinate_flags;
                event.observe = observe;
                recordEvent(event);
                recordingHeaderWritten = true;
            }
            if (!observe && !input.Release()) return 1;
            solver->OnAfeStopped();
            if (packet->flags & HX_RAW_FRAME_SOS) solver->OnAfeSos();
            solver->OnAfeStarted();
            epoch = packet->epoch;
            afeStarted = true;
            frameHealth.Reset();
            std::cerr << "AFE epoch=" << epoch << " project=" << project << '\n';
        }
        solver->SetChargerPlugged(packet->flags & HX_RAW_FRAME_CHARGER);
        if (recorder) {
            *recordedFrame = {};
            recordedFrame->packet = *packet;
            recordedFrame->hostTimeNs = now; // Exact time used by StreamState::Accept.
            recordedFrame->rotation = solverScreen.Rotation();
        }
        const auto error = himax::ToThpMaster(packet->payload, thp);
        const bool wasResetRequested = frameHealth.ResetRequested();
        bool requestReset = frameHealth.Observe(error);
        if (error != himax::FrameError::None) {
            ++bad;
            ++frameErrors[static_cast<size_t>(error)];
            if (requestReset && !wasResetRequested)
                std::cerr << "Raw master recovery requested: "
                          << himax::FrameErrorName(error) << '\n';
        } else {
            lastNs = now;
            frame->rawPtr = thp.data();
            frame->rawLen = thp.size();
            const uint64_t solveStart = NowNs();
            const bool report = solverScreen.Process(*solver, *frame, packet->timestamp_ns / 1000000);
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
            if (recorder) {
                recordedFrame->report = report;
                recordedFrame->status = solver->LastAfeStatus();
                recordedFrame->ctrl = solver->LastCtrlFlags();
                recordedFrame->digest = himax::ContactDigest(*frame);
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
                    const auto actionType = himax::HardwareAction(request.type);
                    if (actionType == HX_RAW_RESET) {
                        std::cerr << "Unknown upstream AFE action\n";
                        return 1;
                    }
                    const bool ok = !observe && capture.Command(epoch, packet->sequence, actionType, request.param);
                    const int commandError = ok ? 0 : errno;
                    if (recorder)
                        recordedFrame->actions[recordedFrame->actionCount++] = {
                            actionType, request.param, observe ? -1 : commandError};
                    if (!observe) {
                        ++actionRequests[actionType];
                        actionSuccess[actionType] += ok;
                    }
                    solver->OnAfeActionExecuted(request, ok);
                    if (!observe && !ok && commandError != ESTALE && commandError != EAGAIN)
                        std::cerr << "AFE action failed: " << std::strerror(commandError) << '\n';
                }
            }
        }
        if (requestReset && !observe) {
            if (!input.Release()) return 1;
            ++resetRequests;
            const bool ok = capture.Command(epoch, packet->sequence, HX_RAW_RESET, 0);
            const int commandError = ok ? 0 : errno;
            if (recorder)
                recordedFrame->actions[recordedFrame->actionCount++] = {
                    HX_RAW_RESET, 0, commandError};
            if (ok) {
                ++resetSuccess;
            } else if (commandError != ESTALE && commandError != EAGAIN) {
                std::cerr << "AFE reset request failed: " << std::strerror(commandError) << '\n';
                exitCode = 1;
            }
        }
        if (recorder) recordEvent(*recordedFrame);
        if (exitCode) break;
    }
    if (!observe) {
        if (!input.Release()) exitCode = 1;
        else releaseOnExit.observe = true;
    }
    if (recorder && !recorder->Close(exitCode == 0)) {
        std::cerr << "Recording incomplete: " << std::strerror(recorder->Error()) << '\n';
        exitCode = 1;
    }
    printStats();
    return exitCode;
}
