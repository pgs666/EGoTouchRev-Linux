// SPDX-License-Identifier: MIT
#include "edge-onset.h"

// Test-only control: the static linker uses this definition instead of loading
// edge-onset.cpp from himax-tsacore. All upstream processing is identical.
namespace himax {
void RecoverEdgeOnset(uint8_t, uint8_t, bool) noexcept {}
}
