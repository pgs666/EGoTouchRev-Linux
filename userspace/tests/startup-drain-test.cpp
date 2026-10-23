// SPDX-License-Identifier: MIT
#include "startup-drain-host.h"
#include "afe-start-host.h"
#include "FrameLayout.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>

static void Check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

struct Fixture {
    struct Operation {
        unsigned channel, operation, value;
        bool operator==(const Operation&) const = default;
    };
    std::vector<Operation> operations;
    unsigned failedReads = 0, reads = 0, opens = 0, failAt = 0;
    unsigned waitMs = 0;
    bool failReopen = false;
    static int Call(void* context, unsigned channel, unsigned operation, unsigned value) {
        auto& self = *static_cast<Fixture*>(context);
        self.operations.push_back({channel, operation, value});
        if (self.failAt == self.operations.size()) return -1;
        if (operation == HX_DRAIN_OPEN) {
            ++self.opens;
            if (self.failReopen && self.opens > 2) return -1;
        }
        if (operation == HX_DRAIN_FRAME) {
            const bool failed = self.failedReads & (1u << self.reads++);
            if (failed) self.waitMs += 200; // Scripted GetFrame timeout, not a real wait.
            return failed ? -1 : 0;
        }
        return 0;
    }
    int Run(unsigned& discarded) {
        hx_startup_drain_bus bus{this, Call};
        return hx_windows_startup_drain(&bus, &discarded);
    }
};

static himax_ts_data NativeFixture() {
    himax_ts_data ts{};
    ts.power = {true, true, false, false, true, false};
    ts.otp_tried = true;
    hx_start_test_now = 1000000000ULL;
    return ts;
}

static void InitNative(himax_ts_data& ts) {
    ts.panel_reinit_work = {&ts, 0};
    ts.charger_work = {&ts, 1};
    ts.op_lock = {&ts};
}

static void NativeComparison() {
    for (unsigned failed = 0; failed < 8; ++failed) for (bool finalKick : {false, true}) {
        Fixture windows;
        for (unsigned i = 0; i < 3; ++i)
            if (failed & (1u << i)) windows.failedReads |= 1u << (2 * i + 1);
        unsigned discarded = 0;
        Check(windows.Run(discarded) == 0, "actual Windows cleanup oracle");
        auto ts = NativeFixture();
        InitNative(ts);
        ts.drain_result = failed & 1 ? -EIO : 0;
        Check(hx_start_once(&ts) == 0 && ts.irq_enabled && !ts.started_events,
              "native opens producer before finishing queue cleanup");
        unsigned attempts = (failed & 1) ? 0 : 1;
        while (attempts < 3) {
            Check(ts.startup.active && !ts.started_events && !ts.calibration &&
                  !ts.rate && !ts.sequence, "no packet or running state escapes before cleanup");
            if (!(failed & (1u << attempts))) {
                Check(hx_startup_frame(&ts), "natural producer packet satisfies next consume");
                ++attempts;
            } else {
                const auto next = attempts + 1;
                const bool kick = next == 3 ? finalKick : !(failed & (1u << next));
                ts.drain_result = kick ? 0 : -EIO;
                hx_start_test_now += 200000000;
                hx_start_or_retry(&ts);
                ++attempts;
                if (next < 3 && kick) ++attempts;
            }
        }
        Check(!ts.startup.active && ts.startup.discarded + 3 == discarded &&
              ts.startup.discarded + ts.startup.timeouts == 3,
              "native master consumes same three success/timeout attempts as unchanged Windows code");
        Check(ts.calibration == 1 && ts.rate == 1 && ts.started_events == 1 &&
              ts.detects == 1 && ts.inits == 1 && ts.drains == 1 + ts.startup.timeouts &&
              !ts.sleeps, "only initial open and timed-out waits kick, without reset or fixed sleeps");
        Check(ts.published == unsigned((failed & 4) && finalKick) && ts.sequence == ts.published,
              "post-final-timeout kick is first normal data, not a fourth discarded packet");
    }
    {
        auto ts = NativeFixture();
        InitNative(ts);
        hx_start_once(&ts);
        const auto reads = ts.drains, scheduled = ts.scheduled;
        const auto epoch = ts.epoch;
        hx_start_test_now += 199000000;
        hx_start_or_retry(&ts);
        Check(ts.drains == reads && ts.startup.remaining == 2 && ts.epoch == epoch &&
              ts.delays[scheduled] == 1, "early/duplicate work reschedules remaining wait, no hardware");
        hx_startup_frame(&ts);
        const auto deadline = ts.startup_deadline_ns;
        hx_start_test_now += 1000000;
        hx_start_or_retry(&ts);
        Check(ts.drains == reads && ts.startup.remaining == 1 &&
              ts.startup_deadline_ns == deadline, "new IRQ restarts next wait, stale timer cannot consume it");
        hx_startup_frame(&ts);
        hx_start_test_now += 200000000;
        hx_start_or_retry(&ts);
        Check(ts.drains == reads && ts.started_events == 1 && ts.inits == 1,
              "late cleanup work leaves the healthy stream intact");
    }
    for (unsigned gate = 0; gate < 6; ++gate) {
        auto ts = NativeFixture();
        InitNative(ts);
        hx_start_once(&ts);
        if (gate == 0) ts.power.streaming = false;
        if (gate == 1) ts.power.panel_prepared = false;
        if (gate == 2) ts.power.panel_enabled = false;
        if (gate == 3) ts.power.system_suspended = true;
        if (gate == 4) ts.power.shutting_down = true;
        if (gate == 5) ts.power.controller_initialized = false;
        hx_start_test_now += 200000000;
        hx_start_or_retry(&ts);
        Check(!ts.startup.active && !ts.startup_deadline_ns && ts.drains == 1 &&
              !ts.calibration && !ts.rate && !ts.started_events && ts.inits == 1,
              "every closed lifecycle gate cancels cleanup before any SPI or command");
    }
    {
        auto ts = NativeFixture();
        InitNative(ts);
        hx_start_once(&ts);
        hx_stream_off(&ts);
        const auto epoch = ts.epoch;
        hx_start_test_now += 200000000;
        hx_start_or_retry(&ts);
        Check(!ts.startup.active && !ts.startup_deadline_ns && ts.epoch == epoch && ts.drains == 1,
              "STREAMOFF cancels producer cleanup and a late callback cannot resurrect the epoch");
    }
}

int main() {
    using Op = Fixture::Operation;
    const std::vector<Op> setup = {
        {0, HX_DRAIN_OPEN, 0}, {1, HX_DRAIN_OPEN, 0},
        {0, HX_DRAIN_VALID, 0}, {0, HX_DRAIN_BLOCK, 1}, {0, HX_DRAIN_TIMEOUT, 200},
        {1, HX_DRAIN_VALID, 0}, {1, HX_DRAIN_BLOCK, 1}, {1, HX_DRAIN_TIMEOUT, 200},
    };
    for (unsigned failed = 0; failed < 64; ++failed) for (bool failReopen : {false, true}) {
        Fixture f;
        f.failedReads = failed;
        f.failReopen = failReopen;
        unsigned discarded = ~0u, failures = 0;
        Check(f.Run(discarded) == 0, "cleanup remains best-effort on frame/reopen failure");
        auto expected = setup;
        for (unsigned i = 0; i < 6; ++i) {
            const unsigned channel = (i & 1) ? 0 : 1;
            const unsigned bytes = channel ? Frame::kSlaveFrameSize : Frame::kMasterFrameSize;
            expected.push_back({channel, HX_DRAIN_FRAME, bytes});
            if (failed & (1u << i)) {
                ++failures;
                expected.push_back({channel, HX_DRAIN_OPEN, 0});
            }
        }
        Check(f.operations == expected, "real Windows opens/configures, then consumes slave/master three times");
        Check(f.reads == 6 && discarded == 6 - failures,
              "three attempts per channel, not three checksum-valid frames");
        Check(f.waitMs == failures * 200 && f.opens == 2 + failures,
              "timeout requests no-reset producer kick without adding a fixed 8ms sleep");
    }
    for (unsigned fail = 1; fail <= setup.size(); ++fail) {
        Fixture f;
        f.failAt = fail;
        unsigned discarded = ~0u;
        Check(f.Run(discarded) != 0 && f.reads == 0 && discarded == ~0u,
              "open/read-policy failure aborts before any frame cleanup");
        std::vector<Op> expected(setup.begin(), setup.begin() + fail);
        if (fail != 1) {
            expected.push_back({1, HX_DRAIN_CLOSE, 0});
            expected.push_back({0, HX_DRAIN_CLOSE, 0});
        }
        Check(f.operations == expected, "read policy or slave-open failure closes both channels");
    }
    NativeComparison();
    std::cout << "Original Windows startup queue drain and native master state machine: "
                 "all read outcomes, failed reopens, bounded waits, late work and cancellation passed\n";
}
