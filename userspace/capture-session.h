// SPDX-License-Identifier: MIT
#pragma once

#include "recording.h"
#include "SolverTypes.h"
#include "hx-v4l2-uapi.h"

#include <atomic>
#include <memory>

namespace himax {

enum class SessionKind { Begin, Frame, State, Release };

struct RecordedAction {
    uint32_t action = 0;
    uint32_t parameter = 0;
    int result = -1;  // 0 success, positive errno, -1 observe-only/not executed.
};

struct SessionEvent {
    SessionKind kind = SessionKind::Frame;
    uint64_t hostTimeNs = 0;
    hx_raw_frame packet{};
    hx_raw_state state{};
    int rotation = -1;
    bool report = false;
    uint32_t status = 0;
    uint32_t ctrl = 0;
    uint64_t digest = 0;
    std::array<RecordedAction, 6> actions{};
    size_t actionCount = 0;
    std::array<char, 16> project{};
    uint32_t coordinateFlags = 0;
    bool observe = false;
};

// Diagnostic fingerprint of mapper output, including UP events and float bits.
// Never hashes structure padding or process addresses.
uint64_t ContactDigest(const Solvers::HeatmapFrame& frame);

// Single producer/single consumer. No allocation, mutex or I/O in TryPush.
class SessionQueue {
public:
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    static constexpr size_t kCapacity = 128;
    bool TryPush(const SessionEvent& event);
    bool TryPop(SessionEvent& event);

private:
    std::array<SessionEvent, kCapacity> m_events{};
    alignas(64) std::atomic<uint64_t> m_head{0};
    alignas(64) std::atomic<uint64_t> m_tail{0};
};

// Creates PATH and PATH.events exclusively. Normal input never waits for disk.
// Overflow/write failure disables recording, preserving input operation.
class SessionRecorder {
public:
    explicit SessionRecorder(const std::string& path);
    ~SessionRecorder();
    bool Submit(const SessionEvent& event);
    bool Close(bool complete);
    int Error() const;
    uint64_t Dropped() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Paired HXRC/HXSE reader. Missing footer, dropped events, trailing raw frames
// and malformed sidecar are errors, even when the raw prefix is readable.
class SessionReader {
public:
    explicit SessionReader(const char* path);
    bool Next(SessionEvent& event);
    const std::string& Error() const { return m_error; }
    const std::string& ProjectId() const { return m_raw.ProjectId(); }
    uint32_t CoordinateFlags() const { return m_raw.CoordinateFlags(); }

private:
    RecordingReader m_raw;
    std::ifstream m_events;
    std::string m_error;
    uint64_t m_frames = 0;
    bool m_begun = false;
    bool m_ended = false;
};

}  // namespace himax
