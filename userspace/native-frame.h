// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "FrameLayout.h"

namespace himax {

inline constexpr size_t kNativeHeaderSize = 4;
inline constexpr size_t kNativeMasterSize =
    kNativeHeaderSize + Frame::kMatrixBytes + Frame::kMasterSuffixBytes;
inline constexpr size_t kNativeTransportSize = 5132;

enum class FrameError {
    None,
    Size,
    Sync,
    Checksum,
};

// The THP API returns three transport bytes before the native event-stack.
// Preserve the native checksum, matrix and suffix byte-for-byte. Do not invent
// a checksum or require a latched calibration marker to classify a valid frame.
FrameError ToThpMaster(std::span<const uint8_t> native,
                      std::array<uint8_t, Frame::kMasterFrameSize>& thp) noexcept;
const char* FrameErrorName(FrameError error) noexcept;

}  // namespace himax
