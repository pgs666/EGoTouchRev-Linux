// SPDX-License-Identifier: MIT
#include "native-frame.h"
#include "edge-onset.h"
#include "solver-screen.h"
#include "Alg/Touch/TouchData.h"
#include "App/Report.h"
#include "App/TsaStatic.h"
#include "Prmt/Prmt.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

int main(int argc, char** argv) {
    if (argc != 2 && argc != 5) return 2;
    const int rotation = std::atoi(argv[1]);
    Check(rotation >= 0 && rotation <= 3, "valid display direction");
    auto solver = std::make_unique<Solvers::Tsa::TsaCoreSolver>();
    auto frame = std::make_unique<Solvers::HeatmapFrame>();
    solver->SetPanelProjectId("W273AS2700");
    himax::SolverScreenState screen;
    screen.SetRotation(rotation);
    std::array<uint8_t, himax::kNativeTransportSize> native{};
    std::array<uint8_t, Frame::kMasterFrameSize> thp{};
    constexpr size_t suffix = 4 + Frame::kMatrixBytes;
    uint64_t timestamp = 1000;
    auto process = [&](double x, double y, bool calibration) {
        native.fill(0);
        native[0] = 0x5a;
        native[1] = 0xa5;
        native[suffix + 108] = native[suffix + 110] = 0xff;
        native[suffix + 6] = calibration ? 0xbb : 0;
        for (int row = 0; row < 40; ++row) {
            for (int col = 0; col < 60; ++col) {
                const double d2 = (col - x) * (col - x) + (row - y) * (row - y);
                const int signal = d2 < 9 ? static_cast<int>(1500 * std::exp(-d2 / 1.0)) : 0;
                const uint16_t raw = 0x7ffe + signal;
                const size_t offset = 4 + 2 * (row * 60 + col);
                native[offset] = raw & 0xff;
                native[offset + 1] = raw >> 8;
            }
        }
        uint32_t sum = 0;
        for (size_t i = 4; i < himax::kNativeMasterSize; i += 2)
            sum += static_cast<uint16_t>(native[i] | native[i + 1] << 8);
        native[2] = -sum & 0xff;
        native[3] = (-sum >> 8) & 0xff;
        if (himax::ToThpMaster(native, thp) != himax::FrameError::None) std::abort();
        frame->rawPtr = thp.data();
        frame->rawLen = thp.size();
        const bool report = screen.Process(*solver, *frame, timestamp);
        timestamp += 9;
        return report;
    };
    auto warmup = [&] {
        solver->OnAfeStopped();
        solver->OnAfeSos();
        solver->OnAfeStarted();
        for (int i = 0; i < 100; ++i) {
            process(-100, -100, i < 3);
            Check(frame->touch.output.contacts.empty(), "flat grid has no contacts");
        }
    };
    warmup();
    if (argc == 2) {
        for (int edge = 0; edge < 4; ++edge) {
            for (double speed : {1.5, 2.0}) {
                for (double drift : {0.0, 0.15, -0.15}) {
                    warmup();
                    int id = -1, moves = 0, firstFrame = -1;
                    float firstDistance = -1;
                    for (int i = 0; i < 10; ++i) {
                        const double d = -1.0 + i * speed;
                        const double x = edge < 2 ? (edge == 0 ? d : 59 - d) : 30 + i * drift;
                        const double y = edge >= 2 ? (edge == 2 ? d : 39 - d) : 20 + i * drift;
                        process(x, y, false);
                        for (const auto& c : frame->touch.output.contacts) {
                            Check(c.reportEvent != Solvers::TouchReportUp,
                                  "edge entry retains contact through inward slide");
                            if (id < 0) {
                                id = c.id;
                                firstFrame = i;
                                firstDistance = edge == 0 ? c.x : edge == 1 ? 2559.9f - c.x :
                                                edge == 2 ? c.y : 1599.9f - c.y;
                            }
                            Check(id == c.id, "edge correction does not create another ID");
                            ++moves;
                        }
                    }
                    Check(firstFrame >= 0 && firstFrame <= 3 && moves >= 6,
                          "edge entry is admitted without additional confirmation frames");
                    if (firstDistance < -0.1f || firstDistance > 1)
                        std::cerr << "rotation=" << rotation << " edge=" << edge
                                  << " speed=" << speed << " drift=" << drift
                                  << " first distance=" << firstDistance << '\n';
                    Check(firstDistance >= -0.1f && firstDistance <= 1,
                          "first report reaches entry edge for fast axis/diagonal swipe");
                }
            }
        }

        // A real interior down/move must stay in the interior through the same
        // adapter, core and output mapper, rather than becoming an edge gesture.
        warmup();
        bool interiorReported = false;
        for (int i = 0; i < 10; ++i) {
            process(20 + i, 20, false);
            for (const auto& c : frame->touch.output.contacts) {
                interiorReported = true;
                Check(c.x > 500 && c.x < 2000 && c.y > 400 && c.y < 1200,
                      "interior down and drag are not pulled to an edge");
            }
        }
        Check(interiorReported, "interior fixture actually reports");

        // Exercise interpolation at physical first/last rows and columns,
        // including clipped corners. The original contour WindowLine reads
        // beyond ZoneSig on the last row even for a zero-weight cubic tap.
        for (double x : {0.0, 30.0, 59.0}) {
            for (double y : {0.0, 20.0, 39.0}) {
                warmup();
                for (int i = 0; i < 10; ++i) process(x, y, false);
            }
        }

        // Exercise rejection conditions on a qualified first-report state.
        // Keep the same raw/track state while asking only whether this one
        // report coordinate may be recovered.
        using namespace Solvers::Tsa::Core;
        auto& tt = Touches();
        auto& pt = PrevTouches();
        tt.count = pt.count = 1;
        tt.exceptionFlags = 0;
        Static().noiseLevel = 0;
        tt.unit[0] = {};
        pt.unit[0] = {};
        auto& c = tt.unit[0];
        auto& p = pt.unit[0];
        p.startPos = {500, 5000};
        p.pos = p.startPos;
        p.startTimeMs = p.timestamp = 1000;
        c.pos = {1300, 5000};
        c.rptPos = c.pos;
        c.timestamp = 1009;
        c.majorMm = c.maxMajorMm = 10;
        c.event = TouchEvent::kTouchDown;
        const auto cur = c;
        const auto prev = p;
        auto unchanged = [&] {
            const auto before = c;
            himax::RecoverEdgeOnset(0, 0, true);
            Check(c.rptPos.dim1 == before.rptPos.dim1 && c.rptPos.dim2 == before.rptPos.dim2,
                  "unqualified start retains upstream coordinate");
            c = cur;
            p = prev;
        };
        himax::RecoverEdgeOnset(0, 0, false);
        Check(c.rptPos.dim1 == cur.rptPos.dim1, "cannot override failed touch admission");
        himax::RecoverEdgeOnset(0, 0, true);
        Check(c.rptPos.dim1 == 0, "qualified axis-aligned first report is recovered");
        Check(c.pos.dim1 == cur.pos.dim1 && c.event == cur.event &&
              c.persistFlags == cur.persistFlags && p.historyCount == prev.historyCount,
              "correction preserves detection/debounce/history/release state");
        c = cur;
        p.persistFlags = 0x100000; unchanged(); // Already reported once.
        c.timestamp = 1040; unchanged();       // Grip-rejected or stale start.
        c.pos = {400, 5000}; unchanged();      // Moving out.
        c.pos = {500, 5500}; unchanged();      // Along edge.
        c.pos = p.startPos; unchanged();       // Stationary edge tap.
        p.startPos = {3000, 5000}; c.pos = {6500, 5000}; unchanged(); // Interior.
        p.startPos.dim2 = 100; unchanged();    // Corner.
        c.majorMm = 40; unchanged();           // Palm.
        c.exceptionType = 0x80; unchanged();
        c.suppressFlags = 2; unchanged();      // Weak signal.
        c.zoneShape = 3; unchanged();          // Split region.
        c.event = TouchEvent::kLiftOff; unchanged();
        p.timestamp = 1005; p.pos = {1400, 5000}; unchanged(); // Reversal.
        tt.count = 2; unchanged(); tt.count = 1; // Multiple current touches.
        pt.count = 2; unchanged(); pt.count = 1;
        tt.exceptionFlags = 1; unchanged(); tt.exceptionFlags = 0;
        Static().noiseLevel = 1; unchanged(); Static().noiseLevel = 0;
        const auto edge = Report::SideToEdge(Report::DimEdgeToSide(1));
        auto& prmt = PrmtFlash().erEdgeMm[edge];
        const auto saved = prmt.minDistMm;
        prmt.minDistMm = 0;
        unchanged();
        prmt.minDistMm = saved;
        std::cout << "edge onset regression passed for rotation " << rotation << '\n';
        return 0;
    }
    const int edge = std::atoi(argv[2]);
    const double speed = std::atof(argv[3]);
    const double drift = std::atof(argv[4]);
    const auto& fl = Solvers::Tsa::Core::PrmtFlash();
    const auto& fc = Solvers::Tsa::Core::PrmtFlashConst();
    std::cout << "profile=" << solver->LoadedPrmtName() << " rotation=" << rotation
              << " logic=" << fc.dim1NearEdge << ',' << fc.dim1FarEdge << ','
              << fc.dim2NearEdge << ',' << fc.dim2FarEdge << '\n';
    for (int i = 0; i < 4; ++i)
        std::cout << "edge=" << i << " mm=" << fl.erEdgeMm[i].minDistMm << ','
                  << fl.erEdgeMm[i].maxDistMm << " enable=" << +fl.erEdgeMm[i].field4
                  << " q8=" << fc.erEdge[i].minDist << ',' << fc.erEdge[i].maxDist << '\n';
    for (int i = 0; i < 20; ++i) {
        const double distance = -1.0 + i * speed;
        const double x = edge < 2 ? (edge == 0 ? distance : 59 - distance) : 30 + i * drift;
        const double y = edge >= 2 ? (edge == 2 ? distance : 39 - distance) : 20 + i * drift;
        process(x, y, false);
        const auto& tt = Solvers::Tsa::Core::Touches();
        const auto& pt = Solvers::Tsa::Core::PrevTouches();
        std::cout << "f=" << i << " raw=" << x << ',' << y << " n=" << +tt.count;
        for (int j = 0; j < pt.count; ++j)
            std::cout << " PRE(id=" << +pt.unit[j].id << " ev=" << pt.unit[j].event
                      << " start=" << pt.unit[j].startPos.dim1 << ',' << pt.unit[j].startPos.dim2
                      << " side=" << pt.unit[j].startSide << " hist=" << +pt.unit[j].historyCount
                      << " db=" << pt.unit[j].debounceFrames << ')';
        for (int j = 0; j < tt.count; ++j) {
            const auto& t = tt.unit[j];
            std::cout << " {id=" << +t.id << " ev=" << t.event << " pos=" << t.pos.dim1
                      << ',' << t.pos.dim2 << " rpt=" << t.rptPos.dim1 << ',' << t.rptPos.dim2
                      << " flags=" << std::hex << t.persistFlags << '/' << t.volatileFlags
                      << '/' << t.suppressFlags << std::dec << " exc=" << t.exceptionType
                      << " major=" << t.majorMm;
            if (t.prevIdx < pt.count)
                std::cout << " db=" << pt.unit[t.prevIdx].debounceFrames;
            std::cout << '}';
        }
        for (const auto& c : frame->touch.output.contacts)
            std::cout << " OUT(" << c.reportEvent << ")=" << c.x << ',' << c.y;
        std::cout << '\n';
    }
}
