// SPDX-License-Identifier: MIT
#include "session-replay.h"
#include "afe-action.h"
#include "capture-session.h"
#include "solver-screen.h"
#include "stream-state.h"

#include <iostream>
#include <memory>

namespace himax {

int ReplaySession(const char* path) {
    SessionReader reader(path);
    if (!reader.Error().empty()) {
        std::cerr << reader.Error() << '\n';
        return 1;
    }
    auto solver = std::make_unique<Solvers::Tsa::TsaCoreSolver>();
    const std::string_view project = reader.ProjectId();
    if (Solvers::Tsa::ResolvePanelProjectId(project) != project || !solver->SetPanelProjectId(project)) {
        std::cerr << "Unknown session panel\n";
        return 2;
    }
    auto frame = std::make_unique<Solvers::HeatmapFrame>();
    auto event = std::make_unique<SessionEvent>();
    SolverScreenState screen;
    StreamState stream;
    std::array<uint8_t, Frame::kMasterFrameSize> thp{};
    uint64_t epoch = 0, frames = 0, validFrames = 0, lastTime = 0;
    bool started = false, observe = false;
    MasterFrameHealth frameHealth;
    std::cout << "frame,time_ms,afe_status,ctrl,report,id,event,x,y,pressure\n";
    while (reader.Next(*event)) {
        const auto& e = *event;
        if (e.kind == SessionKind::Begin) { observe = e.observe; continue; }
        if (e.kind == SessionKind::Release) continue; // Input-only timeout; mapper is unchanged.
        if (e.kind == SessionKind::State) {
            if (!stream.State(e.state, e.hostTimeNs)) {
                std::cerr << "Invalid session lifecycle event\n";
                return 1;
            }
            if (!e.state.running || e.state.epoch != epoch) {
                solver->OnAfeStopped();
                started = false;
            }
            continue;
        }
        ++frames;
        const auto& packet = e.packet;
        if (packet.timestamp_ns < lastTime || !stream.Accept(packet, e.hostTimeNs)) {
            std::cerr << "Invalid session timeline\n";
            return 1;
        }
        lastTime = packet.timestamp_ns;
        if (packet.epoch != epoch || !started) {
            solver->OnAfeStopped();
            if (packet.flags & HX_RAW_FRAME_SOS) solver->OnAfeSos();
            solver->OnAfeStarted();
            epoch = packet.epoch;
            started = true;
            frameHealth.Reset();
        }
        screen.SetRotation(e.rotation);
        solver->SetChargerPlugged(packet.flags & HX_RAW_FRAME_CHARGER);
        const auto error = ToThpMaster(packet.payload, thp);
        const bool valid = error == FrameError::None;
        bool reset = frameHealth.Observe(error);
        bool report = false;
        if (!valid) {
            if (e.report || e.status || e.ctrl || e.digest) {
                std::cerr << "Unexpected solver result on invalid frame " << frames << '\n';
                return 1;
            }
        } else {
            ++validFrames;
            frame->rawPtr = thp.data();
            frame->rawLen = thp.size();
            report = screen.Process(*solver, *frame, packet.timestamp_ns / 1000000);
            if (!solver->IsAvailable()) return 1;
            reset = solver->LastAfeStatus() & Solvers::Tsa::AfeStatus::kSos;
            if (report != e.report || solver->LastAfeStatus() != e.status ||
                solver->LastCtrlFlags() != e.ctrl || ContactDigest(*frame) != e.digest) {
                std::cerr << "Session solver output mismatch at frame " << frames << '\n';
                return 1;
            }
            for (const auto& t : frame->touch.output.contacts)
                std::cout << frames << ',' << packet.timestamp_ns / 1000000 << ','
                          << solver->LastAfeStatus() << ',' << solver->LastCtrlFlags() << ','
                          << report << ',' << t.id << ',' << t.reportEvent << ',' << t.x << ','
                          << t.y << ',' << t.signalSum << '\n';
        }
        size_t index = 0;
        const auto matches = [&](hx_raw_action action, uint32_t parameter) {
            return index < e.actionCount && e.actions[index].action == action &&
                   e.actions[index].parameter == parameter &&
                   (observe ? e.actions[index].result == -1 : e.actions[index].result >= 0);
        };
        if (!reset && valid) {
            for (const auto& action : solver->PendingAfeActions()) {
                if (!matches(HardwareAction(action.type), action.param)) {
                    std::cerr << "Session AFE request mismatch at frame " << frames << '\n';
                    return 1;
                }
                solver->OnAfeActionExecuted(action, e.actions[index++].result == 0);
            }
        }
        if (reset && !observe) {
            if (!matches(HX_RAW_RESET, 0)) {
                std::cerr << "Session reset mismatch at frame " << frames << '\n';
                return 1;
            }
            ++index;
        }
        if (index != e.actionCount) {
            std::cerr << "Unexpected session command at frame " << frames << '\n';
            return 1;
        }
    }
    if (!reader.Error().empty()) {
        std::cerr << reader.Error() << '\n';
        return 1;
    }
    std::cerr << "session_frames=" << frames << " verified_frames=" << validFrames
              << " live_output_verified=" << !!validFrames
              << " profile=" << solver->LoadedPrmtName() << '\n';
    return validFrames ? 0 : 1;
}

}  // namespace himax
