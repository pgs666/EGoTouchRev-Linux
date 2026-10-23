// SPDX-License-Identifier: MIT
#include "native-frame.h"
#include "hx-v4l2-uapi.h"

#include <algorithm>

namespace himax {

static_assert(kNativeMasterSize == HX_RAW_MASTER_SIZE);
static_assert(kNativeTransportSize == HX_RAW_PAYLOAD_SIZE);

FrameError ToThpMaster(std::span<const uint8_t> native,
                      std::array<uint8_t, Frame::kMasterFrameSize>& thp) noexcept {
    if (native.size() != kNativeMasterSize && native.size() != kNativeTransportSize)
        return FrameError::Size;
    // Check the checksum body, as Windows CheckFrame does. The sync bytes may
    // also be filler, and the transport's trailing 72 bytes are not master data.
    // Only scan for filler on its distinctive first word, keeping the ordinary
    // conversion path to a single checksum pass.
    if (native[2] == 0xec && native[3] == 0xec &&
        std::all_of(native.begin() + 2, native.begin() + kNativeMasterSize,
                    [](uint8_t byte) { return byte == 0xec; }))
        return FrameError::BusFiller;

    uint32_t sum = 0;
    for (size_t i = 2; i < kNativeMasterSize; i += 2)
        sum += static_cast<uint16_t>(native[i] | (native[i + 1] << 8));
    if (sum == 0)
        return FrameError::AllZero;
    if (native[0] != 0x5a || native[1] != 0xa5)
        return FrameError::Sync;
    if ((sum & 0xffffu) != 0)
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
    case FrameError::AllZero: return "all-zero";
    case FrameError::BusFiller: return "bus-filler";
    case FrameError::Count: break;
    }
    return "unknown";
}

}  // namespace himax
