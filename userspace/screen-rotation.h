// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace himax {

inline constexpr std::string_view kScreenRotationSocket = "himax-touchd-screen-state";
inline constexpr std::string_view kScreenBridgeSocketPrefix = "himax-screen-bridge-";
inline constexpr std::string_view kScreenRotationRequest = "HXSR 1\n";

// Explicit TSACore state, not a KScreen/XRandR bit mask.
std::optional<int> ParseScreenRotation(std::string_view text) noexcept;
std::optional<int> ParseKScreenRotation(int mask) noexcept;

struct ScreenRotationSample {
    int value = 0;
    std::string source;
};

// The unprivileged desktop bridge publishes over a nonblocking Unix datagram
// socket. SO_PASSCRED limits updates to root or the active local seat user.
// fb0/rotate is deliberately excluded: it is not the compositor orientation.
class ScreenRotationProvider {
public:
    explicit ScreenRotationProvider(
        std::string rotationFile = {},
        std::string_view socketName = kScreenRotationSocket,
        std::string_view bridgeSocketPrefix = kScreenBridgeSocketPrefix);
    ~ScreenRotationProvider();
    ScreenRotationProvider(const ScreenRotationProvider&) = delete;
    ScreenRotationProvider& operator=(const ScreenRotationProvider&) = delete;

    // fixedRotation, when present, is useful for replay and deterministic
    // tests.  Otherwise the first readable source wins.
    void SetFixed(std::optional<int> fixedRotation) noexcept;
    // One nonblocking request before STREAMON. A running desktop bridge can
    // answer from its recent cache while the hardware initializes.
    bool RequestInitial() const;
    [[nodiscard]] std::optional<ScreenRotationSample> Poll() const;

private:
    std::string m_rotationFile;
    std::optional<int> m_fixed;
    std::string m_bridgeSocketPrefix;
    int m_socket = -1;
};

}  // namespace himax
