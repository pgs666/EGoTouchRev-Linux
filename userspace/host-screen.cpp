// SPDX-License-Identifier: MIT
#include "host-screen.h"
#include "TsaCore.h"

namespace himax {
namespace {
thread_local int g_rotation = -1;
}

ScopedScreenRotation::ScopedScreenRotation(int rotation) noexcept : m_previous(g_rotation) {
    g_rotation = rotation >= 0 && rotation <= 3 ? rotation : -1;
}

ScopedScreenRotation::~ScopedScreenRotation() {
    g_rotation = m_previous;
}

void ApplyHostScreenRotation() noexcept {
    if (g_rotation >= 0) Solvers::Tsa::Core::SetScreenRotateState(g_rotation);
}

}  // namespace himax
