// SPDX-License-Identifier: MIT
#include "native-frame.h"
#include "recording.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>

himax::FrameError WindowsMasterCheck(const uint8_t* data);

static void Check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

static himax::FrameError Compare(std::span<const uint8_t> native) {
    std::array<uint8_t, Frame::kMasterFrameSize> windows{}, actual{};
    std::copy_n(native.begin(), himax::kNativeMasterSize, windows.begin() + 3);
    actual.fill(0xab);
    const auto before = actual;
    const auto expected = WindowsMasterCheck(windows.data());
    const auto result = himax::ToThpMaster(native, actual);
    Check(result == expected, "native classification equals actual Windows CheckFrame");
    Check(result == himax::FrameError::None ? actual == windows : actual == before,
          "accepted bytes preserved; failed conversion retains last evidence");
    return result;
}

static void Checksum(std::span<uint8_t> frame) {
    uint32_t sum = 0;
    for (size_t i = 4; i < himax::kNativeMasterSize; i += 2)
        sum += static_cast<uint16_t>(frame[i] | (frame[i + 1] << 8));
    const auto value = static_cast<uint16_t>(0u - sum);
    frame[2] = value & 0xff;
    frame[3] = value >> 8;
}

int main(int argc, char** argv) {
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            himax::RecordingReader reader(argv[i], himax::RecordingFormat::Hxst, 0);
            himax::RecordedFrame frame;
            unsigned frames = 0, valid = 0, nonMagic = 0, admitted = 0;
            while (reader.Next(frame)) {
                const auto result = Compare({frame.payload.data(), frame.length});
                ++frames;
                valid += result == himax::FrameError::None;
                if (frame.payload[0] != 0x5a || frame.payload[1] != 0xa5) {
                    ++nonMagic;
                    admitted += result == himax::FrameError::None;
                }
            }
            Check(reader.Error().empty() && frames, "read complete nonempty capture");
            std::cout << argv[i] << ": frames=" << frames << " valid=" << valid
                      << " non_magic=" << nonMagic << " admitted_non_magic=" << admitted << '\n';
        }
        return 0;
    }
    std::array<uint8_t, himax::kNativeTransportSize> native{};
    native[0] = 0x5a;
    native[1] = 0xa5;
    Check(Compare(native) == himax::FrameError::AllZero, "zero checksum body rejected");
    for (size_t i = 4; i < himax::kNativeMasterSize; ++i) native[i] = i * 17 + 5;
    Checksum(native);
    // Neither Windows acquisition nor its adapter treats the first two native
    // bytes as a fixed sync marker. Exercise every possible value of that word.
    for (unsigned header = 0; header < 65536; ++header) {
        native[0] = header & 0xff;
        native[1] = header >> 8;
        Check(Compare({native.data(), himax::kNativeMasterSize}) == himax::FrameError::None,
              "checksum-valid master accepted for every header word");
    }
    for (size_t i = 2; i < himax::kNativeMasterSize; ++i) {
        native[i] ^= 1;
        Check(Compare(native) == himax::FrameError::Checksum, "each body byte covered by checksum");
        native[i] ^= 1;
    }
    for (size_t i = himax::kNativeMasterSize; i < native.size(); ++i) native[i] = 0xec;
    Check(Compare(native) == himax::FrameError::None, "legacy extra tail is not checksum evidence");
    native.fill(0xec);
    Check(Compare(native) == himax::FrameError::BusFiller, "complete filler causes immediate SOS");
    native[10] ^= 1;
    Check(Compare(native) == himax::FrameError::Checksum, "partial filler is ordinary corruption");
    uint32_t random = 0x48121;
    for (unsigned n = 0; n < 512; ++n) {
        for (auto& byte : native) {
            random = random * 1664525u + 1013904223u;
            byte = random >> 24;
        }
        Compare(native);
        Checksum(native);
        Check(Compare(native) == himax::FrameError::None, "repaired random checksum accepted");
    }
    std::array<uint8_t, Frame::kMasterFrameSize> output{};
    for (size_t size : {0u, 1u, 2u, 5059u, 5061u, 5131u})
        Check(himax::ToThpMaster({native.data(), size}, output) == himax::FrameError::Size,
              "Linux size guard runs before fixed-size Windows oracle boundary");
    std::cout << "Windows frame-check oracle: all header words, every body byte, filler, "
                 "random grids, legacy padding and size guards passed\n";
}
