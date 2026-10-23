// SPDX-License-Identifier: MIT
#pragma once
#include "capture-session.h"

#include <atomic>
#include <thread>

namespace himax {

// Producer methods run on the solver thread. All filesystem operations and
// SessionRecorder construction/draining happen on the management thread.
class LiveCapture {
public:
    LiveCapture(std::string directory, unsigned seconds);
    ~LiveCapture();
    bool Start(const SessionEvent& begin, uint64_t now);
    void Stop();
    void Tick(uint64_t now);
    bool Submit(const SessionEvent& event);
    bool Recording() const { return m_recording && !m_error.load(); }
    bool Busy() const { return m_busy.load(std::memory_order_acquire); }
    int Error() const { return m_error.load(); }

private:
    void Wake() const;
    void Run();
    void Fail(int error);
    std::string m_directory;
    unsigned m_seconds;
    SessionQueue m_queue;
    std::atomic<int> m_error{0};
    std::atomic<bool> m_busy{false}, m_stopping{false};
    std::atomic<uint64_t> m_dropped{0};
    std::thread m_worker;
    int m_wake = -1;
    bool m_recording = false, m_endPending = false;
    uint64_t m_deadline = 0;
};

} // namespace himax
