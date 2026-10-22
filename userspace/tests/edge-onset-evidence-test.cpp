// SPDX-License-Identifier: MIT
#include "edge-onset.h"
#include "TsaCore.h"
#include "Alg/Touch/TouchData.h"
#include "App/Report.h"
#include "App/TsaBuffer.h"
#include "App/TsaStatic.h"
#include "Prmt/Prmt.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>

using namespace Solvers::Tsa::Core;

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

enum class Case {
    Entry, NoPulse, Weak, Broad, TwoPeaks, InteriorPeak, Remote, TwoEdges,
    Stationary, Noisy, Reset, BaselineReset, ExistingTouch, Exception,
    MissingQuiet, MissingCreation, Gap, TooSlow, Cleared, ClockRewind,
};

static bool Recover(Case test) {
    himax::ResetEdgeOnset();
    auto& stat = Static();
    auto& prev = PrevTouches();
    auto& cur = Touches();
    const auto& fl = PrmtFlash();
    const auto& fc = PrmtFlashConst();
    const int column = fl.dim1LogicFirst;
    const int row = 19;
    auto pulse = [&] {
        for (int r = row - 1; r <= row + 1; ++r)
            Buffers().dif[GridIndex(column, r)] = r == row ? 800 : 300;
    };
    auto observe = [&](uint64_t time, uint64_t before) {
        stat.curTimeMs = time;
        stat.prevTimeMs = before;
        himax::ObserveEdgeOnset();
    };
    stat.systemFlags = stat.noiseLevel = 0;
    Prpt().blStage = 0;
    cur = {};
    prev = {};
    std::fill_n(Buffers().dif, fl.sensorNum, 0);
    if (test == Case::Stationary) pulse();
    if (test != Case::MissingQuiet) observe(991, 982);
    pulse();
    if (test == Case::NoPulse) std::fill_n(Buffers().dif, fl.sensorNum, 0);
    if (test == Case::Weak) Buffers().dif[GridIndex(column, row)] = 399;
    if (test == Case::Broad)
        for (int r = row - 3; r <= row + 3; ++r)
            Buffers().dif[GridIndex(column, r)] = 800;
    if (test == Case::TwoPeaks) Buffers().dif[GridIndex(column, row + 5)] = 800;
    if (test == Case::InteriorPeak) Buffers().dif[GridIndex(column + 1, row)] = 1000;
    if (test == Case::Remote) {
        std::fill_n(Buffers().dif, fl.sensorNum, 0);
        Buffers().dif[GridIndex(column, row + 5)] = 800;
    }
    if (test == Case::TwoEdges) Buffers().dif[GridIndex(fl.dim1LogicLast, row)] = 800;
    if (test == Case::Noisy) stat.noiseLevel = 1;
    if (test == Case::Reset) stat.systemFlags = 1;
    if (test == Case::BaselineReset) Prpt().blStage = 1;
    if (test == Case::ExistingTouch) prev.count = 1;
    if (test == Case::Exception) prev.exceptionFlags = 1;
    observe(1000, 991);
    stat.systemFlags = stat.noiseLevel = 0;
    Prpt().blStage = 0;
    prev = {};
    std::fill_n(Buffers().dif, fl.sensorNum, 0);
    const uint64_t creationTime = test == Case::Gap ? 1027 : 1009;
    if (test != Case::MissingCreation) observe(creationTime, 1000);
    if (test == Case::Cleared) himax::ResetEdgeOnset();
    if (test == Case::ClockRewind) observe(900, 891);

    cur.count = prev.count = 1;
    auto& c = cur.unit[0];
    auto& p = prev.unit[0];
    const int profile = Report::SideToEdge(Report::DimEdgeToSide(1));
    Check(PrmtFlash().erEdgeMm[profile].minDistMm != 0, "fixture edge enabled");
    const int distance = fc.erEdge[profile].maxDist + 100;
    const int first = fc.dim1NearEdge + distance;
    p.startPos = p.pos = {static_cast<uint16_t>(first), 5000};
    p.startTimeMs = p.timestamp = creationTime;
    c.pos = c.rptPos = {static_cast<uint16_t>(first + distance), 5000};
    c.timestamp = creationTime + (test == Case::TooSlow ? 18 : 9);
    c.majorMm = c.maxMajorMm = 10;
    c.event = TouchEvent::kTouchDown;
    himax::RecoverEdgeOnset(0, 0, true);
    Check(c.pos.dim1 == first + distance && c.event == TouchEvent::kTouchDown &&
          c.persistFlags == 0 && prev.unit[0].historyCount == 0,
          "evidence recovery changes only report position");
    return c.rptPos.dim1 == fc.dim1NearEdge;
}

int main() {
    constexpr uint16_t frequencies[] = {100, 120};
    constexpr uint16_t rates[] = {120};
    AfeCaps caps{60, 40, 2, frequencies, 1, rates};
    Check(Init("W273AS2700", caps), "core fixture initialized");
    PrmtDynamic().edgePeakThold = 400;
    Check(Recover(Case::Entry), "fresh clipped boundary pulse recovers distant onset");
    for (const auto test : {
             Case::NoPulse, Case::Weak, Case::Broad, Case::TwoPeaks, Case::InteriorPeak,
             Case::Remote, Case::TwoEdges, Case::Stationary, Case::Noisy, Case::Reset,
             Case::BaselineReset, Case::ExistingTouch, Case::Exception, Case::MissingQuiet,
             Case::MissingCreation, Case::Gap, Case::TooSlow, Case::Cleared, Case::ClockRewind})
        Check(!Recover(test), "ambiguous/stale/unrelated boundary evidence is rejected");
    std::cout << "Boundary evidence: entry accepted; 19 rejection/lifecycle cases passed\n";
}
