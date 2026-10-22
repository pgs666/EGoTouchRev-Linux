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
    AllZero,
    BusFiller,
    Count,
};

// HimaxChipFrame.cpp treats an unresponsive bus filled with 0xEC as immediate
// SOS. Other invalid master frames retain the five-frame recovery threshold.
// Shared by the live host and strict replay; never runs in the kernel.
class MasterFrameHealth {
public:
    static constexpr unsigned int kBadFrameLimit = 5;

    void Reset() noexcept { m_bad = 0; m_reset = false; }
    bool Observe(FrameError error) noexcept {
        if (error == FrameError::None) {
            Reset();
        } else {
            if (m_bad < kBadFrameLimit) ++m_bad;
            m_reset |= error == FrameError::BusFiller || m_bad == kBadFrameLimit;
        }
        return m_reset;
    }
    [[nodiscard]] bool ResetRequested() const noexcept { return m_reset; }

private:
    unsigned int m_bad = 0;
    bool m_reset = false;
};

// The THP API returns three transport bytes before the native event-stack.
// Preserve the native checksum, matrix and suffix byte-for-byte. Do not invent
// a checksum or require a latched calibration marker to classify a valid frame.
FrameError ToThpMaster(std::span<const uint8_t> native,
                      std::array<uint8_t, Frame::kMasterFrameSize>& thp) noexcept;
const char* FrameErrorName(FrameError error) noexcept;

}  // namespace himax
