// SPDX-License-Identifier: MIT
#pragma once

#include "TsaCoreSolver.h"
#include "host-screen.h"

namespace himax {

// The screen belongs to the Linux host, not the optional pen channel.
// The checked upstream integration applies it after initialization and inside
// reset, before the same frame's direction-dependent touch/grip processing.
class SolverScreenState {
public:
    void SetRotation(int rotation) noexcept { m_rotation = rotation; }
    int Rotation() const noexcept { return m_rotation; }

    bool Process(Solvers::Tsa::TsaCoreSolver& solver, Solvers::HeatmapFrame& frame, uint64_t timeMs) const {
        const ScopedScreenRotation rotation(m_rotation);
        return solver.Process(frame, timeMs);
    }

private:
    int m_rotation = -1;
};

}  // namespace himax
