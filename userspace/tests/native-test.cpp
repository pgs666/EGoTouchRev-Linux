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

int main(int argc, char** argv) {
    Check(argc == 1 || argc == 3, "profile and expected parameter name supplied together");
    const std::string_view project = argc == 3 ? argv[1] : "W273AS2700";
    const std::string_view expected = argc == 3 ? argv[2] : "W273AS2700";
    std::array<uint8_t, himax::kNativeTransportSize> native{};
    std::array<uint8_t, Frame::kMasterFrameSize> thp{};
    Check(himax::ToThpMaster(native, thp) == himax::FrameError::AllZero, "all zero classified");
    native.fill(0xec);
    native.back() = 0; // Only the master body determines whether the bus is filler.
    Check(himax::ToThpMaster(native, thp) == himax::FrameError::BusFiller,
          "unresponsive bus classified even with invalid sync");
    native[0] = 0x5a;
    native[1] = 0xa5;
    Check(himax::ToThpMaster(native, thp) == himax::FrameError::BusFiller,
          "filler body with sync also classified");
    native[himax::kNativeMasterSize - 1] ^= 1;
    Check(himax::ToThpMaster(native, thp) == himax::FrameError::Checksum,
          "partial filler remains an ordinary checksum error");
    native.fill(0);
    native[0] = 0x5a;
    native[1] = 0xa5;
    Check(himax::ToThpMaster(native, thp) == himax::FrameError::AllZero,
          "zero body with sync is not a valid checksum");
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
    const auto lastGood = thp;
    Check(himax::ToThpMaster(native, thp) == himax::FrameError::Checksum, "corruption rejected");
    Check(thp == lastGood, "failed conversion preserves previous complete THP buffer");
    native[40] ^= 1;
    native[0] ^= 1;
    Check(himax::ToThpMaster(native, thp) == himax::FrameError::None,
          "checksum-valid body does not require a fixed header, matching Windows");
    native[0] ^= 1;

    himax::MasterFrameHealth health;
    for (const auto error : {himax::FrameError::Sync, himax::FrameError::Checksum,
                             himax::FrameError::AllZero, himax::FrameError::Size})
        Check(!health.Observe(error), "first four mixed bad frames do not reset");
    Check(health.Observe(himax::FrameError::Checksum), "fifth bad frame requests recovery");
    for (unsigned int i = 0; i < 10000; ++i)
        Check(health.Observe(himax::FrameError::AllZero), "bad run cannot overflow retry state");
    Check(!health.Observe(himax::FrameError::None), "valid frame clears bad run");
    Check(health.Observe(himax::FrameError::BusFiller), "one bus-filler frame requests immediate SOS");
    Check(health.Observe(himax::FrameError::Sync), "failed immediate reset remains pending");
    health.Reset();
    Check(!health.ResetRequested() && !health.Observe(himax::FrameError::Checksum),
          "new AFE epoch starts with fresh recovery state");
    Check(!health.Observe(himax::FrameError::None) &&
          !health.Observe(himax::FrameError::AllZero), "good frame breaks consecutive error run");

    // Full upstream initialization and 240 frames including IDLE entry/exit and
    // calibration completion with its latched marker subsequently cleared.
    auto solver = std::make_unique<Solvers::Tsa::TsaCoreSolver>();
    Check(solver->SetPanelProjectId(project), "requested panel identity selected");
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
    Check(solver->PanelProjectId() == project &&
          std::string_view(solver->LoadedPrmtName()) == expected, "correct panel parameters loaded");
    std::cout << "native conversion and full TSACore smoke test passed\n";
}
