// SPDX-License-Identifier: MIT
#include "touch-trace.h"

#include "Alg/Touch/TouchData.h"
#include "App/Report.h"
#include "App/TsaStatic.h"
#include "Prmt/Prmt.h"

#include <ostream>

namespace himax {

void WriteTouchTraceHeader(std::ostream& out) {
    out << "frame,epoch,sequence,time_ms,rotation,core_index,core_id,core_time_ms,"
           "start_time_ms,event,age,debounce_frames,history_count,persist_flags,"
           "suppress_flags,exception_type,major_mm,start_dim1,start_dim2,raw_dim1,"
           "raw_dim2,pos_dim1,pos_dim2,rpt_dim1,rpt_dim2,first_rpt_dim1,first_rpt_dim2,"
           "zone_min_dim1,zone_max_dim1,zone_min_dim2,zone_max_dim2,start_edge,"
           "start_edge_dist,edge_max_dist,reported_once,dim1_near,dim1_far,"
           "dim2_near,dim2_far,zone_shape,noise_level,frame_exception_flags,"
           "edge_enabled,max_major_mm\n";
}

void WriteTouchTrace(std::ostream& out, uint64_t frame, uint64_t epoch,
                     uint64_t sequence, uint64_t timeMs, int rotation) {
    using namespace Solvers::Tsa::Core;
    const auto& prev = PrevTouches();
    const auto& fc = PrmtFlashConst();
    for (uint8_t i = 0; i < prev.count && i < kMaxTouchNum; ++i) {
        const auto& p = prev.unit[i];
        const auto edge = Report::GetDimEdge(p.startPos.dim1, p.startPos.dim2);
        const auto profile = Report::SideToEdge(Report::DimEdgeToSide(edge));
        const bool dim1 = edge == 1 || edge == 2;
        const bool far = edge == 2 || edge == 8;
        const int first = dim1 ? p.startPos.dim1 : p.startPos.dim2;
        const int near = dim1 ? fc.dim1NearEdge : fc.dim2NearEdge;
        const int last = (dim1 ? fc.dim1FarEdge : fc.dim2FarEdge) - 1;
        const int distance = far ? last - first : first - near;
        out << frame << ',' << epoch << ',' << sequence << ',' << timeMs << ',' << rotation
            << ',' << +i << ',' << +p.id << ',' << p.timestamp << ',' << p.startTimeMs
            << ',' << p.event << ',' << p.age << ',' << p.debounceFrames << ',' << +p.historyCount
            << ',' << p.persistFlags << ',' << p.suppressFlags << ',' << p.exceptionType
            << ',' << p.majorMm << ',' << p.startPos.dim1 << ',' << p.startPos.dim2
            << ',' << p.rawPos.dim1 << ',' << p.rawPos.dim2 << ',' << p.pos.dim1 << ',' << p.pos.dim2
            << ',' << p.rptPos.dim1 << ',' << p.rptPos.dim2
            << ',' << p.firstRptPos.dim1 << ',' << p.firstRptPos.dim2
            << ',' << +p.zoneMinDim1 << ',' << +p.zoneMaxDim1
            << ',' << +p.zoneMinDim2 << ',' << +p.zoneMaxDim2
            << ',' << edge << ',' << distance << ',' << fc.erEdge[profile].maxDist
            << ',' << !!(p.persistFlags & 0x100000)
            << ',' << fc.dim1NearEdge << ',' << fc.dim1FarEdge
            << ',' << fc.dim2NearEdge << ',' << fc.dim2FarEdge
            << ',' << p.zoneShape << ',' << +Static().noiseLevel << ',' << prev.exceptionFlags
            << ',' << !!PrmtFlash().erEdgeMm[profile].minDistMm << ',' << p.maxMajorMm << '\n';
    }
}

}  // namespace himax
