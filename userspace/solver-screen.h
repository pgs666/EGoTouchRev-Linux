// SPDX-License-Identifier: MIT
#pragma once

#include "TsaCoreSolver.h"

namespace himax {

// The screen belongs to the Linux host, not the optional pen channel.
// Use the public Core API without modifying the pinned upstream submodule.
class SolverScreenState {
public:
    void SetRotation(int rotation) noexcept { m_rotation = rotation; }
    int Rotation() const noexcept { return m_rotation; }

    bool Process(Solvers::Tsa::TsaCoreSolver& solver, Solvers::HeatmapFrame& frame, uint64_t timeMs) const {
        Apply(solver);
        const bool report = solver.Process(frame, timeMs);
        // Upstream MsReset -> Sys::Init resets screenRotation to zero. Restore
        // host state after reset frames as well as before ordinary frames.
        // No input flush, mapper reset, baseline reset or AFE restart here.
        Apply(solver);
        return report;
    }

private:
    void Apply(const Solvers::Tsa::TsaCoreSolver& solver) const {
        if (m_rotation >= 0 && solver.IsAvailable())
            Solvers::Tsa::Core::SetScreenRotateState(m_rotation);
    }
    int m_rotation = -1;
};

}  // namespace himax
