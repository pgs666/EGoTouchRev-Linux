// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include "hx-v4l2-uapi.h"

namespace himax {

inline bool ValidPayloadSize(uint32_t size) noexcept {
    return size == HX_RAW_MASTER_SIZE || size == HX_RAW_PAYLOAD_SIZE;
}

// Old transports captured all 5132 bytes. New transports capture the complete
// 5060-byte master and zero the unused record capacity, never inventing evidence.
inline bool ValidPayload(const hx_raw_frame& frame) noexcept {
    return ValidPayloadSize(frame.payload_size) &&
        std::all_of(std::begin(frame.payload) + frame.payload_size, std::end(frame.payload),
                    [](unsigned char value) { return value == 0; });
}

}  // namespace himax
