// SPDX-License-Identifier: MIT
#pragma once

namespace himax {

// The solver is a process singleton. An override belongs only to the current
// Process call on its owning thread, including initialization and reset.
class ScopedScreenRotation {
public:
    explicit ScopedScreenRotation(int rotation) noexcept;
    ~ScopedScreenRotation();
    ScopedScreenRotation(const ScopedScreenRotation&) = delete;
    ScopedScreenRotation& operator=(const ScopedScreenRotation&) = delete;

private:
    int m_previous;
};

// Used by the two checked build-time upstream integration points. With no
// host override (-1), leave upstream behavior unchanged.
void ApplyHostScreenRotation() noexcept;

}  // namespace himax
