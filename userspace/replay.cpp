// SPDX-License-Identifier: MIT
#include "native-frame.h"
#include "recording.h"
#include "TsaCoreSolver.h"
#include "solver-screen.h"
#include "session-replay.h"

#include <array>
#include <charconv>
#include <fstream>
#include <iostream>
#include <memory>
#include <string_view>

int main(int argc, char** argv) {
    himax::RecordingFormat format = himax::RecordingFormat::Native;
    bool startupGrace = false;
    bool session = false;
    himax::SolverScreenState screen;
    while (argc > 1) {
        const std::string_view option(argv[1]);
        if (option == "--allow-startup-invalid") startupGrace = true;
        else if (option == "--session") session = true;
        else if (option == "--trace") format = himax::RecordingFormat::Trace;
        else if (option == "--hxst") format = himax::RecordingFormat::Hxst;
        else if (option == "--rotation" && argc > 2) {
            const std::string_view value(argv[2]);
            int rotation = -1;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), rotation);
            if (error != std::errc{} || end != value.data() + value.size() || rotation < 0 || rotation > 3) {
                std::cerr << "Replay rotation must be 0..3\n";
                return 2;
            }
            screen.SetRotation(rotation);
            --argc;
            ++argv;
        } else break;
        --argc;
        ++argv;
    }
    if (argc < 2 || argc > 4) {
        std::cerr << "Usage: himax-replay [--allow-startup-invalid] [--rotation 0..3] [--trace|--hxst]\n"
                  << "                    FRAMES [PROJECT_ID] [RECORD_BYTES]\n"
                  << "       himax-replay --session CAPTURE.hxst\n"
                  << "Input: concatenated 5132-byte native frames (or 5060 with RECORD_BYTES).\n";
        return 2;
    }
    if (session) {
        if (argc != 2 || startupGrace || format != himax::RecordingFormat::Native ||
            screen.Rotation() >= 0) {
            std::cerr << "--session requires recorded identity, orientation and strict validation\n";
            return 2;
        }
        return himax::ReplaySession(argv[1]);
    }
    size_t recordBytes = himax::kNativeTransportSize;
    if (argc == 4) {
        const std::string_view arg(argv[3]);
        const auto [end, error] = std::from_chars(arg.data(), arg.data() + arg.size(), recordBytes);
        if (error != std::errc{} || end != arg.data() + arg.size() ||
            (recordBytes != himax::kNativeTransportSize && recordBytes != himax::kNativeMasterSize)) {
            std::cerr << "RECORD_BYTES must be 5060 or 5132\n";
            return 2;
        }
    }
    himax::RecordingReader input(argv[1], format, recordBytes);
    if (!input.Error().empty()) {
        std::cerr << input.Error() << ": " << argv[1] << '\n';
        return 1;
    }
    auto solver = std::make_unique<Solvers::Tsa::TsaCoreSolver>();
    const std::string_view requested = argc >= 3 ? std::string_view(argv[2]) : input.ProjectId();
    if (!requested.empty()) {
        const auto project = Solvers::Tsa::ResolvePanelProjectId(requested);
        if (project != requested || !solver->SetPanelProjectId(project)) {
            std::cerr << "Unknown project id\n";
            return 2;
        }
        if (argc >= 3 && !input.ProjectId().empty() && project != input.ProjectId())
            std::cerr << "Overriding recorded project " << input.ProjectId() << " with " << project << '\n';
    }
    auto frame = std::make_unique<Solvers::HeatmapFrame>();
    himax::RecordedFrame record;
    std::array<uint8_t, Frame::kMasterFrameSize> thp{};
    uint64_t sequence = 0;
    size_t rejected = 0, allowed = 0, valid = 0;
    unsigned int initialRejected = 0;
    bool epochStarted = false;
    uint64_t epoch = 0;
    uint64_t lastTime = 0;
    std::cout << "frame,time_ms,afe_status,ctrl,report,id,event,x,y,pressure\n";
    while (input.Next(record)) {
        ++sequence;
        if (!record.readOk) continue;
        if (lastTime > record.timeNs) {
            std::cerr << "Timestamp went backwards\n";
            return 1;
        }
        lastTime = record.timeNs;
        if (record.epoch != epoch) {
            solver->OnAfeStopped();
            if (record.sos) solver->OnAfeSos();
            solver->OnAfeStarted();
            epoch = record.epoch;
            epochStarted = false;
            initialRejected = 0;
        }
        solver->SetChargerPlugged(record.charger);
        const auto error = himax::ToThpMaster(
            std::span(record.payload.data(), record.length), thp);
        if (error != himax::FrameError::None) {
            ++rejected;
            // AFE calibration can leave a short startup transient. Keep
            // recording/logging every rejected frame, and never tolerate
            // corruption after the first valid frame or a five-frame run.
            if (startupGrace && !epochStarted && ++initialRejected <= 4)
                ++allowed;
            std::cerr << "frame " << sequence << ": " << himax::FrameErrorName(error) << '\n';
            continue;
        }
        epochStarted = true;
        ++valid;
        frame->rawPtr = thp.data();
        frame->rawLen = thp.size();
        const uint64_t timeMs = record.timeNs / 1000000;
        const bool report = screen.Process(*solver, *frame, timeMs);
        if (!solver->IsAvailable()) {
            std::cerr << "TSACore initialization failed\n";
            return 1;
        }
        for (const auto& touch : frame->touch.output.contacts) {
            std::cout << sequence << ',' << timeMs << ',' << solver->LastAfeStatus() << ','
                      << solver->LastCtrlFlags() << ',' << report << ',' << touch.id << ','
                      << touch.reportEvent << ',' << touch.x << ',' << touch.y << ','
                      << touch.signalSum << '\n';
        }
    }
    if (!input.Error().empty()) {
        std::cerr << input.Error() << '\n';
        return 1;
    }
    std::cerr << "frames=" << sequence << " rejected=" << rejected
              << " startup_allowed=" << allowed
              << " profile=" << solver->LoadedPrmtName() << '\n';
    return valid && rejected == allowed ? 0 : 1;
}
