// SPDX-License-Identifier: MIT
#include "native-frame.h"
#include "TsaCoreSolver.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>

static void Check(bool condition, const char* name) {
    if (!condition) {
        std::cerr << "FAIL: " << name << '\n';
        std::exit(1);
    }
}

static void Checksum(std::array<uint8_t, himax::kNativeTransportSize>& frame) {
    uint32_t sum = 0;
    for (size_t i = 4; i < himax::kNativeMasterSize; i += 2)
        sum += static_cast<uint16_t>(frame[i] | (frame[i + 1] << 8));
    const uint16_t checksum = static_cast<uint16_t>(0u - sum);
    frame[2] = checksum & 0xff;
    frame[3] = checksum >> 8;
}

int main() {
    std::array<uint8_t, himax::kNativeTransportSize> native{};
    std::array<uint8_t, Frame::kMasterFrameSize> thp{};
    Check(himax::ToThpMaster(native, thp) == himax::FrameError::Sync, "all zero rejected");
    native[0] = 0x5a;
    native[1] = 0xa5;
    for (size_t i = 4; i < 4 + Frame::kMatrixBytes; i += 2) {
        native[i] = 0xfe;
        native[i + 1] = 0x7f;
    }
    constexpr size_t suffix = 4 + Frame::kMatrixBytes;
    native[suffix + 108] = 0xff;
    native[suffix + 110] = 0xff;
    Checksum(native);
    Check(himax::ToThpMaster(native, thp) == himax::FrameError::None, "native checksum valid");
    Check(std::equal(native.begin(), native.begin() + himax::kNativeMasterSize, thp.begin() + 3),
          "transport bytes preserved");
    Check(himax::ToThpMaster(std::span(native.data(), 99), thp) == himax::FrameError::Size,
          "short input rejected");
    native[40] ^= 1;
    Check(himax::ToThpMaster(native, thp) == himax::FrameError::Checksum, "corruption rejected");
    native[40] ^= 1;

    // Full upstream initialization and 240 frames including IDLE entry/exit and
    // calibration completion with its latched marker subsequently cleared.
    auto solver = std::make_unique<Solvers::Tsa::TsaCoreSolver>();
    auto frame = std::make_unique<Solvers::HeatmapFrame>();
    for (uint64_t i = 0; i < 240; ++i) {
        native[suffix] = i >= 80 && i < 100 ? 1 : 0;
        native[suffix + 6] = i >= 120 && i < 124 ? 0xbb : 0;
        Checksum(native);
        Check(himax::ToThpMaster(native, thp) == himax::FrameError::None, "status transitions valid");
        frame->rawPtr = thp.data();
        frame->rawLen = thp.size();
        solver->Process(*frame, 1000 + i * 1000 / 120);
        Check(solver->IsAvailable(), "full TSACore initialized");
        Check(solver->GetCachedContactCount() == 0, "flat grid creates no contacts");
    }
    Check(std::string_view(solver->LoadedPrmtName()) == "W273AS2700", "CSOT profile selected");
    std::cout << "native conversion and full TSACore smoke test passed\n";
}
