// SPDX-License-Identifier: MIT
#include "native-frame.h"

#include <algorithm>

namespace himax {

FrameError ToThpMaster(std::span<const uint8_t> native,
                      std::array<uint8_t, Frame::kMasterFrameSize>& thp) noexcept {
    if (native.size() != kNativeMasterSize && native.size() != kNativeTransportSize)
        return FrameError::Size;
    if (native[0] != 0x5a || native[1] != 0xa5)
        return FrameError::Sync;

    uint32_t sum = 0;
    for (size_t i = 2; i < kNativeMasterSize; i += 2)
        sum += static_cast<uint16_t>(native[i] | (native[i + 1] << 8));
    if (sum == 0 || (sum & 0xffffu) != 0)
        return FrameError::Checksum;

    thp.fill(0);
    std::copy_n(native.begin(), kNativeMasterSize, thp.begin() + 3);
    return FrameError::None;
}

const char* FrameErrorName(FrameError error) noexcept {
    switch (error) {
    case FrameError::None: return "ok";
    case FrameError::Size: return "size";
    case FrameError::Sync: return "sync";
    case FrameError::Checksum: return "checksum";
    }
    return "unknown";
}

}  // namespace himax
