// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace himax {

// Small, process-local history of clean boundary signals before peak admission.
// The core is a singleton. Reset with it; observe after Prmt::Process, before
// peak detection. No raw/baseline or touch state is changed.
void ResetEdgeOnset() noexcept;
void ObserveEdgeOnset() noexcept;

// Called after upstream admission and filtering, before the first report point
// is recorded. Changes only that report's coordinates, never touch admission,
// tracking, raw data, baseline, debounce or the history replay queue.
void RecoverEdgeOnset(uint8_t current, uint8_t previous, bool admitted) noexcept;

}  // namespace himax
