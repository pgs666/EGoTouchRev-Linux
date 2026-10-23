// SPDX-License-Identifier: MIT
#pragma once
#include <chrono>
#include <cstdint>
#include <cstdlib>

struct TestClock {
    inline static uint64_t time = 0;
    static auto now() { return std::chrono::steady_clock::time_point(std::chrono::milliseconds(time)); }
};
static inline void TestSleep(std::chrono::milliseconds delay) { TestClock::time += delay.count(); }
struct HalDevice {
    unsigned *schedule;
    unsigned capacity, count, timeout;
    bool IsValid() const { return true; }
    bool IsCancelledError() const { return false; }
    unsigned GetError() const { return 1460; }
    bool GetFrame(uint8_t *, uint32_t, void *) { TestClock::time += timeout; return false; }
    bool IntOpen() {
        if (count >= capacity) std::abort();
        schedule[count++] = TestClock::time;
        return true;
    }
};
// Consume the real logging arguments without a formatting library.
template<class... T> void TestLog(const T&...) {}
static inline const char* GetStateStr() { return "test"; }
#define LOG_NOTICE(...) TestLog(__VA_ARGS__)
