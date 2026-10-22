// SPDX-License-Identifier: MIT
#include "edge-onset.h"

#include "Alg/Touch/TouchData.h"
#include "App/Report.h"
#include "App/TsaStatic.h"
#include "Prmt/Prmt.h"

#include <algorithm>
#include <cstdlib>

namespace himax {

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

    // Only a recent start inside this panel's existing ER band, followed by a
    // predominantly inward slide whose backward extension reaches that edge.
    // Slow/stationary/along-edge starts, corners and long jumps stay upstream.
    if (maxDistance <= 0 || distance < 0 || distance >= maxDistance ||
        inward < std::max(minInward, distance) || inward > 4 * maxDistance ||
        std::abs(tangent) > inward ||
        firstOther < otherNear + cornerMargin || firstOther > otherLast - cornerMargin ||
        now < near || now > last || nowOther < otherNear || nowOther > otherLast)
        return;

    // If there are three observed positions, reject reversal or a sudden turn.
    // For a one-frame debounce, prev.pos is the first sample itself.
    if (prev.timestamp > prev.startTimeMs) {
        const int before = alongDim1 ? prev.pos.dim1 : prev.pos.dim2;
        const int beforeOther = alongDim1 ? prev.pos.dim2 : prev.pos.dim1;
        const int step = far ? before - now : now - before;
        const int priorStep = far ? first - before : before - first;
        if (step < 50 || priorStep < 50 || std::abs(nowOther - beforeOther) > step ||
            std::abs(beforeOther - firstOther) > priorStep)
            return;
    }
    const int intercept = firstOther - static_cast<int>(
        static_cast<int64_t>(tangent) * distance / inward);
    if (intercept < otherNear + cornerMargin || intercept > otherLast - cornerMargin)
        return;
    if (alongDim1) {
        cur.rptPos = {static_cast<uint16_t>(boundary), static_cast<uint16_t>(intercept)};
    } else {
        cur.rptPos = {static_cast<uint16_t>(intercept), static_cast<uint16_t>(boundary)};
    }
}

}  // namespace himax
