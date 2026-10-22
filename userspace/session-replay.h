// SPDX-License-Identifier: MIT
#pragma once

#include <iosfwd>

namespace himax {

int ReplaySession(const char* path, std::ostream* touchTrace = nullptr);

}  // namespace himax
