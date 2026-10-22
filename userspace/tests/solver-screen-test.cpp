// SPDX-License-Identifier: MIT
#include "native-frame.h"
#include "screen-rotation.h"
#include "solver-screen.h"
#include "App/Buffer.h"
#include "App/Sys.h"
#include "App/Tsa.h"
#include "App/TsaBuffer.h"
#include "App/TsaStatic.h"
#include "Rejection/GripFilter/GripFilter.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

static void Check(bool pass, const char* message) {
    if (!pass) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

static void Checksum(std::array<uint8_t, himax::kNativeTransportSize>& native) {
    uint32_t sum = 0;
    for (size_t i = 4; i < himax::kNativeMasterSize; i += 2)
        sum += static_cast<uint16_t>(native[i] | native[i + 1] << 8);
    native[2] = -sum & 0xff;
    native[3] = (-sum >> 8) & 0xff;
}

int main(int argc, char** argv) {
    Check(argc == 2, "one initial rotation");
    const auto initial = himax::ParseScreenRotation(argv[1]);
    Check(initial.has_value(), "valid initial rotation");
    auto solver = std::make_unique<Solvers::Tsa::TsaCoreSolver>();
    auto frame = std::make_unique<Solvers::HeatmapFrame>();
    himax::SolverScreenState screen;
    screen.SetRotation(*initial);
    std::array<uint8_t, himax::kNativeTransportSize> native{};
    std::array<uint8_t, Frame::kMasterFrameSize> thp{};
    native[0] = 0x5a;
    native[1] = 0xa5;
    for (size_t i = 4; i < 4 + Frame::kMatrixBytes; i += 2) {
        native[i] = 0xfe;
        native[i + 1] = 0x7f;
    }
    constexpr size_t suffix = 4 + Frame::kMatrixBytes;
    native[suffix + 108] = native[suffix + 110] = 0xff;
    solver->OnAfeStarted();
    uint64_t timestamp = 1000;
    for (int rotation : {*initial, 1, 2, 3, 0}) {
        screen.SetRotation(rotation);
        for (int i = 0; i < 80; ++i) {
            // Exercise the persistent marker, its clear and subsequent epochs
            // while preserving the selected display direction.
            if (i == 40) {
                solver->OnAfeStopped();
                solver->OnAfeStarted();
            }
            native[suffix + 6] = i < 3 || (i >= 40 && i < 43) ? 0xbb : 0;
            native[suffix] = i >= 60 && i < 70 ? 1 : 0;
            Checksum(native);
            Check(himax::ToThpMaster(native, thp) == himax::FrameError::None, "valid frame");
            frame->rawPtr = thp.data();
            frame->rawLen = thp.size();
            screen.Process(*solver, *frame, timestamp);
            timestamp += 9;
            Check(solver->IsAvailable(), "solver initialized");
            Check(Solvers::Tsa::Core::Static().screenRotation == rotation,
                  "host direction survives initialization/calibration/IDLE/AFE epochs");
            Check(solver->GetCachedContactCount() == 0, "flat grid has no ghosts");
        }
        const auto* baseline = Solvers::Tsa::Core::Buffers().bl;
        std::array<int16_t, Frame::kMatrixCells> saved{};
        std::copy_n(baseline, saved.size(), saved.begin());
        screen.SetRotation((rotation + 1) % 4);
        Check(std::equal(saved.begin(), saved.end(), baseline),
              "direction update itself never replaces baseline");
    }
    // A compact interior finger must remain the same contact while host
    // orientation changes. This checks the live mapper, not just flat grids.
    native[suffix] = native[suffix + 6] = 0;
    for (int row = 19; row <= 21; ++row) {
        for (int col = 29; col <= 31; ++col) {
            const uint16_t signal = row == 20 && col == 30 ? 1000 : 500;
            const uint16_t raw = 0x7ffe + signal;
            const size_t offset = 4 + 2 * (row * 60 + col);
            native[offset] = raw & 0xff;
            native[offset + 1] = raw >> 8;
        }
    }
    int contactId = -1;
    unsigned int moves = 0;
    for (int i = 0; i < 120; ++i) {
        if (i % 20 == 0) screen.SetRotation((i / 20 + *initial) % 4);
        Checksum(native);
        Check(himax::ToThpMaster(native, thp) == himax::FrameError::None, "valid finger frame");
        const bool report = screen.Process(*solver, *frame, timestamp);
        timestamp += 9;
        Check(Solvers::Tsa::Core::Static().screenRotation == screen.Rotation(),
              "host direction survives active contact");
        for (const auto& contact : frame->touch.output.contacts) {
            Check(contact.reportEvent != Solvers::TouchReportUp,
                  "direction change does not release interior finger");
            if (contactId < 0) contactId = contact.id;
            Check(contact.id == contactId, "direction change preserves tracking ID");
            Check(report, "active contact requests input report");
            ++moves;
        }
    }
    Check(contactId >= 0 && moves >= 100, "real solver reports sustained synthetic finger");

    // Check the actual reset functions before Process can return and a wrapper
    // could hide an incorrect direction. Build the real grip map immediately.
    for (int rotation = 0; rotation < 4; ++rotation) {
        const himax::ScopedScreenRotation scope(rotation);
        Solvers::Tsa::Core::Sys::Init();
        Check(Solvers::Tsa::Core::Static().screenRotation == rotation,
              "Sys::Init applies the host direction internally");
        Solvers::Tsa::Core::Tsa::MsReset();
        Check(Solvers::Tsa::Core::Static().screenRotation == rotation,
              "MsReset preserves host direction before any later frame processing");
        Solvers::Tsa::Core::GripFilter::RegionProcess();
        std::array<uint8_t, Frame::kMatrixCells> resetRegion{};
        std::copy_n(Solvers::Tsa::Core::Buffers().gripRegion, resetRegion.size(), resetRegion.begin());
        Solvers::Tsa::Core::SetScreenRotateState(0);
        Solvers::Tsa::Core::GripFilter::RegionProcess();
        if (rotation != 0)
            Check(!std::equal(resetRegion.begin(), resetRegion.end(),
                              Solvers::Tsa::Core::Buffers().gripRegion),
                  "nonzero orientation produces a genuinely different grip region");
        Solvers::Tsa::Core::SetScreenRotateState(rotation);
        Solvers::Tsa::Core::GripFilter::RegionProcess();
        Check(std::equal(resetRegion.begin(), resetRegion.end(),
                         Solvers::Tsa::Core::Buffers().gripRegion),
              "reset-frame grip map matches explicit host direction");
    }
    {
        const himax::ScopedScreenRotation outer(3);
        {
            const himax::ScopedScreenRotation unknown(-1);
            Solvers::Tsa::Core::Sys::Init();
            Check(Solvers::Tsa::Core::Static().screenRotation == 0,
                  "no host direction leaves upstream reset default intact");
        }
        Solvers::Tsa::Core::Sys::Init();
        Check(Solvers::Tsa::Core::Static().screenRotation == 3, "nested scope restores override");
        Solvers::Tsa::Core::SetScreenRotateState(0);
        std::thread other([] { himax::ApplyHostScreenRotation(); });
        other.join();
        Check(Solvers::Tsa::Core::Static().screenRotation == 0,
              "another thread never inherits the solver-thread override");
        himax::ApplyHostScreenRotation();
        Check(Solvers::Tsa::Core::Static().screenRotation == 3,
              "owning thread retains its override");
    }
    Solvers::Tsa::Core::Sys::Init();
    Check(Solvers::Tsa::Core::Static().screenRotation == 0,
          "host override cannot leak into a later unscoped solver session");
    std::cout << "solver screen state regression passed\n";
}
