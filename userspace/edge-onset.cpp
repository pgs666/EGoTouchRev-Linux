// SPDX-License-Identifier: MIT
#include "edge-onset.h"

#include "Alg/Baseline/Baseline.h"
#include "Alg/Touch/TouchData.h"
#include "App/Report.h"
#include "App/Sys.h"
#include "App/TsaBuffer.h"
#include "App/TsaStatic.h"
#include "Prmt/Prmt.h"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace himax {
namespace {
using namespace Solvers::Tsa::Core;

struct BoundarySignal {
    bool present = false;
    bool clippedPeak = false;
    int tangent = 0;
};

struct BoundaryFrame {
    bool valid = false;
    bool idle = false;
    uint64_t time = 0;
    uint64_t previousTime = 0;
    std::array<BoundarySignal, 4> edges{};
};

// At most 36 ms of debounce plus two preceding frames. Fixed storage and only
// a perimeter scan; neither recording nor another confirmation frame is needed.
std::array<BoundaryFrame, 8> boundaryFrames{};
size_t nextBoundaryFrame = 0;
constexpr uint64_t maxBoundaryIntervalMs = 18;

const BoundaryFrame* FindBoundaryFrame(uint64_t time) {
    for (const auto& frame : boundaryFrames)
        if (frame.valid && frame.time == time) return &frame;
    return nullptr;
}

BoundarySignal ReadBoundary(int edge) {
    const auto& fl = PrmtFlash();
    const auto& dyn = PrmtDynamic();
    const bool dim1 = edge < 2;
    const bool far = (edge & 1) != 0;
    const int boundary = dim1 ? (far ? fl.dim1LogicLast : fl.dim1LogicFirst) :
                               (far ? fl.dim2LogicLast : fl.dim2LogicFirst);
    const int inside = boundary + (far ? -1 : 1);
    const int first = dim1 ? fl.dim2LogicFirst : fl.dim1LogicFirst;
    const int last = dim1 ? fl.dim2LogicLast : fl.dim1LogicLast;
    // noiseThold is the core's adaptive peak/baseline threshold, not a noise
    // amplitude estimate. A clipped entry can be below fingerThold (the very
    // reason no candidate exists yet); use the existing edge-peak threshold.
    const int peakThreshold = std::max(1, static_cast<int>(dyn.edgePeakThold));
    const int threshold = std::max(1, peakThreshold / 2);
    auto signal = [&](int normal, int tangent) {
        return Buffers().dif[dim1 ? GridIndex(normal, tangent) : GridIndex(tangent, normal)];
    };
    int count = 0, runs = 0, peak = 0, innerPeak = 0;
    int64_t weight = 0, moment = 0;
    bool previous = false;
    for (int t = first; t <= last; ++t) {
        const int value = signal(boundary, t);
        const bool hit = value >= threshold;
        if (hit) {
            if (!previous) ++runs;
            ++count;
            peak = std::max(peak, value);
            innerPeak = std::max<int>(innerPeak, signal(inside, t));
            weight += value;
            moment += static_cast<int64_t>(value) * (t * 256 + 128);
        }
        previous = hit;
    }
    BoundarySignal result;
    result.present = count != 0;
    // A compact, single patch whose maximum is clipped at the boundary.
    // Broad palms, separate peaks and peaks already inside the panel are not
    // evidence of an entry from outside.
    result.clippedPeak = runs == 1 && count <= 5 &&
        peak >= peakThreshold &&
        peak - innerPeak >= threshold;
    if (weight != 0) result.tangent = static_cast<int>(moment / weight);
    return result;
}

bool HasBoundaryEntry(const PrevTouchUnit& start, const TouchUnit& current,
                      uint32_t edge, int distance, int inward, int intercept) {
    const auto* creation = FindBoundaryFrame(start.startTimeMs);
    if (!creation || !creation->idle) return false;
    const auto* entry = FindBoundaryFrame(creation->previousTime);
    if (!entry || !entry->idle || creation->time <= entry->time ||
        creation->time - entry->time > maxBoundaryIntervalMs)
        return false;
    const auto* quiet = FindBoundaryFrame(entry->previousTime);
    if (!quiet || !quiet->idle || entry->time <= quiet->time ||
        entry->time - quiet->time > maxBoundaryIntervalMs)
        return false;
    const size_t index = edge == 1 ? 0 : edge == 2 ? 1 : edge == 4 ? 2 : 3;
    const auto& signal = entry->edges[index];
    if (!signal.clippedPeak || quiet->edges[index].present ||
        std::abs(signal.tangent - intercept) > 256)
        return false;
    // Reject simultaneous boundary activity, including broad/ambiguous patches
    // on other sides. Do not attach an unrelated edge signal to an interior down.
    for (size_t i = 0; i < entry->edges.size(); ++i)
        if (i != index && entry->edges[i].present) return false;

    // The observed inward velocity must reach this boundary within the one
    // acquisition interval preceding candidate creation. No distant attraction.
    return static_cast<int64_t>(distance) * (current.timestamp - start.startTimeMs) <=
           static_cast<int64_t>(inward) * (creation->time - entry->time);
}
}  // namespace

void ResetEdgeOnset() noexcept {
    boundaryFrames = {};
    nextBoundaryFrame = 0;
}

void ObserveEdgeOnset() noexcept {
    const auto& stat = Static();
    const auto& fl = PrmtFlash();
    if (nextBoundaryFrame != 0 &&
        stat.curTimeMs <= boundaryFrames[(nextBoundaryFrame - 1) % boundaryFrames.size()].time)
        ResetEdgeOnset();
    auto& frame = boundaryFrames[nextBoundaryFrame++ % boundaryFrames.size()];
    frame = {};
    frame.time = stat.curTimeMs;
    frame.previousTime = stat.prevTimeMs;
    // Invalid/bypassed frames leave a gap in the time-linked history. Resets
    // and baseline transitions cannot provide boundary evidence.
    if (Sys::IsHwReset() || Baseline::IsReset() || stat.noiseLevel != 0 ||
        PrevTouches().exceptionFlags != 0 || !Buffers().dif ||
        fl.dim1LogicFirst >= fl.dim1LogicLast || fl.dim1LogicLast >= fl.dim1Len ||
        fl.dim2LogicFirst >= fl.dim2LogicLast || fl.dim2LogicLast >= fl.dim2Len)
        return;
    frame.valid = true;
    frame.idle = PrevTouches().count == 0;
    if (frame.idle)
        for (int edge = 0; edge < 4; ++edge) frame.edges[edge] = ReadBoundary(edge);
}

void RecoverEdgeOnset(uint8_t current, uint8_t previous, bool admitted) noexcept {
    using namespace Solvers::Tsa::Core;
    constexpr uint32_t reportedOnce = 0x100000;
    constexpr uint32_t suppressedWeakOrStylus = 0x2 | 0x4 | 0x20;
    constexpr uint32_t removedOrSteppedBack = 0x10000 | 0x800000;
    constexpr uint64_t maxOnsetMs = 36;
    constexpr int minInward = 100;  // Q8 sensor units, as in upstream ER.
    constexpr int cornerMargin = 256;

    auto& touches = Touches();
    const auto& previousTouches = PrevTouches();
    if (!admitted || touches.count != 1 || previousTouches.count != 1 ||
        current >= touches.count || previous >= previousTouches.count ||
        touches.exceptionFlags != 0 || Static().noiseLevel != 0)
        return;
    auto& cur = touches.unit[current];
    const auto& prev = previousTouches.unit[previous];
    if ((prev.persistFlags & reportedOnce) != 0 ||
        (cur.persistFlags & removedOrSteppedBack) != 0 ||
        (cur.suppressFlags & suppressedWeakOrStylus) != 0 ||
        cur.exceptionType != 0 || cur.zoneShape == 2 || cur.zoneShape == 3 ||
        cur.majorMm > 29 || cur.maxMajorMm > 29 ||
        (cur.event != TouchEvent::kTouchDown && cur.event != TouchEvent::kMove) ||
        cur.timestamp <= prev.startTimeMs || cur.timestamp - prev.startTimeMs > maxOnsetMs)
        return;

    const auto& fc = PrmtFlashConst();
    const auto& fl = PrmtFlash();
    const auto start = prev.startPos;
    const uint32_t edge = Report::GetDimEdge(start.dim1, start.dim2);
    const int profileEdge = Report::SideToEdge(Report::DimEdgeToSide(edge));
    if (profileEdge < 0 || profileEdge >= 4 || fl.erEdgeMm[profileEdge].minDistMm == 0)
        return;
    const int maxDistance = fc.erEdge[profileEdge].maxDist;
    const bool alongDim1 = edge == 1 || edge == 2;
    const bool far = edge == 2 || edge == 8;
    const int near = alongDim1 ? fc.dim1NearEdge : fc.dim2NearEdge;
    const int last = (alongDim1 ? fc.dim1FarEdge : fc.dim2FarEdge) - 1;
    const int otherNear = alongDim1 ? fc.dim2NearEdge : fc.dim1NearEdge;
    const int otherLast = (alongDim1 ? fc.dim2FarEdge : fc.dim1FarEdge) - 1;
    const int first = alongDim1 ? start.dim1 : start.dim2;
    const int firstOther = alongDim1 ? start.dim2 : start.dim1;
    const int now = alongDim1 ? cur.pos.dim1 : cur.pos.dim2;
    const int nowOther = alongDim1 ? cur.pos.dim2 : cur.pos.dim1;
    const int boundary = far ? last : near;
    const int distance = far ? last - first : first - near;
    const int inward = far ? first - now : now - first;
    const int tangent = nowOther - firstOther;

    // Only a recent start followed by a predominantly inward slide whose
    // backward extension reaches that edge. Starts beyond the existing ER
    // band also need the boundary evidence checked below.
    // Slow/stationary/along-edge starts, corners and long jumps stay upstream.
    if (maxDistance <= 0 || distance < 0 ||
        inward < std::max(minInward, distance) || inward > 4 * maxDistance ||
        std::abs(tangent) > inward ||
        firstOther < otherNear + cornerMargin || firstOther > otherLast - cornerMargin ||
        now < near || now > last || nowOther < otherNear || nowOther > otherLast)
        return;

    // If there are three observed positions, reject reversal or a sudden turn.
    // Edge-clamped samples can share a normal coordinate until admission:
    // their zero normal step is not a reversal. Only accept that plateau at
    // the exact boundary, and bound its tangential drift by the inward step.
    // Interior plateaus and sub-threshold inward motion remain rejected.
    if (prev.timestamp > prev.startTimeMs) {
        const int before = alongDim1 ? prev.pos.dim1 : prev.pos.dim2;
        const int beforeOther = alongDim1 ? prev.pos.dim2 : prev.pos.dim1;
        const int step = far ? before - now : now - before;
        const int priorStep = far ? first - before : before - first;
        const bool boundaryPlateau = distance == 0 && priorStep == 0;
        if (step < 50 || (priorStep < 50 && !boundaryPlateau) ||
            std::abs(nowOther - beforeOther) > step ||
            std::abs(beforeOther - firstOther) > (boundaryPlateau ? step : priorStep))
            return;
    }
    const int intercept = firstOther - static_cast<int>(
        static_cast<int64_t>(tangent) * distance / inward);
    if (intercept < otherNear + cornerMargin || intercept > otherLast - cornerMargin)
        return;
    // Beyond the existing narrow ER band, require an actual clipped boundary
    // pulse immediately before the first candidate, preceded by a quiet frame.
    if (distance >= maxDistance &&
        !HasBoundaryEntry(prev, cur, edge, distance, inward, intercept))
        return;
    if (alongDim1) {
        cur.rptPos = {static_cast<uint16_t>(boundary), static_cast<uint16_t>(intercept)};
    } else {
        cur.rptPos = {static_cast<uint16_t>(intercept), static_cast<uint16_t>(boundary)};
    }
}

}  // namespace himax
