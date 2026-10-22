// SPDX-License-Identifier: MIT
#include "native-frame.h"
#include "recording.h"
#include "TsaCoreSolver.h"

#include <array>
#include <charconv>
#include <fstream>
#include <iostream>
#include <memory>
#include <string_view>

int main(int argc, char** argv) {
    himax::RecordingFormat format = himax::RecordingFormat::Native;
    if (argc > 1 && std::string_view(argv[1]) == "--trace") {
        format = himax::RecordingFormat::Trace;
        --argc;
        ++argv;
    } else if (argc > 1 && std::string_view(argv[1]) == "--hxst") {
        format = himax::RecordingFormat::Hxst;
        --argc;
        ++argv;
    }
    if (argc < 2 || argc > 4) {
        std::cerr << "Usage: himax-replay [--trace|--hxst] FRAMES [PROJECT_ID] [RECORD_BYTES]\n"
                  << "Input: concatenated 5132-byte native frames (or 5060 with RECORD_BYTES).\n";
        return 2;
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
    if (argc >= 3) {
        const auto project = Solvers::Tsa::ResolvePanelProjectId(argv[2]);
        if (project != argv[2] || !solver->SetPanelProjectId(project)) {
            std::cerr << "Unknown project id\n";
            return 2;
        }
    }
    auto frame = std::make_unique<Solvers::HeatmapFrame>();
    himax::RecordedFrame record;
    std::array<uint8_t, Frame::kMasterFrameSize> thp{};
    uint64_t sequence = 0;
    size_t rejected = 0;
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
        }
        solver->SetChargerPlugged(record.charger);
        const auto error = himax::ToThpMaster(
            std::span(record.payload.data(), record.length), thp);
        if (error != himax::FrameError::None) {
            ++rejected;
            std::cerr << "frame " << sequence << ": " << himax::FrameErrorName(error) << '\n';
            continue;
        }
        frame->rawPtr = thp.data();
        frame->rawLen = thp.size();
        const uint64_t timeMs = record.timeNs / 1000000;
        const bool report = solver->Process(*frame, timeMs);
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
              << " profile=" << solver->LoadedPrmtName() << '\n';
    return sequence && !rejected ? 0 : 1;
}
