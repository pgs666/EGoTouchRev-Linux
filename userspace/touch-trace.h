// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <iosfwd>

namespace himax {

// Offline only: inspect the history table AFTER Process() has committed it.
// This is not an observation inside detection/admission and does not label a
// touch as originating outside the screen. Never changes solver state.
void WriteTouchTraceHeader(std::ostream& out);
void WriteTouchTrace(std::ostream& out, uint64_t frame, uint64_t epoch,
                     uint64_t sequence, uint64_t timeMs, int rotation);

}  // namespace himax
