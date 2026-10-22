// SPDX-License-Identifier: MIT
#include "native-frame.h"
#include "TsaCoreSolver.h"

#include <array>
#include <charconv>
#include <fstream>
#include <iostream>
#include <memory>
#include <string_view>

int main(int argc, char** argv) {
    if (argc < 2 || argc > 4) {
        std::cerr << "Usage: himax-replay NATIVE_FRAMES [PROJECT_ID] [RECORD_BYTES]\n"
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
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        std::cerr << "Cannot open recording: " << argv[1] << '\n';
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
    std::array<uint8_t, himax::kNativeTransportSize> native{};
    std::array<uint8_t, Frame::kMasterFrameSize> thp{};
    uint64_t sequence = 0;
    size_t rejected = 0;
    std::cout << "frame,time_ms,afe_status,ctrl,report,id,event,x,y,pressure\n";
    while (input.read(reinterpret_cast<char*>(native.data()), recordBytes)) {
        ++sequence;
        const auto error = himax::ToThpMaster(std::span(native.data(), recordBytes), thp);
        if (error != himax::FrameError::None) {
            ++rejected;
            std::cerr << "frame " << sequence << ": " << himax::FrameErrorName(error) << '\n';
            continue;
        }
        frame->rawPtr = thp.data();
        frame->rawLen = thp.size();
        const uint64_t timeMs = sequence * 1000 / 120;
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
    if (!input.eof() || input.gcount() != 0) {
        std::cerr << "Truncated or unreadable frame at end of recording\n";
        return 1;
    }
    std::cerr << "frames=" << sequence << " rejected=" << rejected
              << " profile=" << solver->LoadedPrmtName() << '\n';
    return sequence && !rejected ? 0 : 1;
}
