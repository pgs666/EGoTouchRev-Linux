// SPDX-License-Identifier: MIT
#include "afe-start-host.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <iostream>

static void Check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
struct Fixture {
    himax_ts_data ts{};
    Fixture() {
        ts.power = {true, true, false, false, true, false};
        ts.otp_tried = true;
        ts.epoch = 7;
        ts.sequence = 123;
        ts.last_command_ns[HX_RAW_RESET] = 987654321;
        ts.last_command_ns[HX_RAW_CALIBRATE] = 42;
        ts.panel_reinit_work = {&ts, 0};
        ts.charger_work = {&ts, 1};
        ts.op_lock = {&ts};
        ts.read_errors = 3;
        ts.charger.applied_valid = true;
    }
};

int main() {
    hx_raw_retry_state policy{};
    for (unsigned i = 1; i < 10000; ++i)
        Check(hx_raw_retry_failed(&policy) == hx_windows_backoff(i) && policy.failures == i,
              "retry delay matches unchanged Windows BringUpBackoff");
    policy.failures = UINT_MAX - 1;
    Check(hx_raw_retry_failed(&policy) == 2000 && policy.failures == UINT_MAX &&
          hx_raw_retry_failed(&policy) == 2000 && policy.failures == UINT_MAX,
          "permanent failure count saturates instead of wrapping into fast retries");
    hx_raw_retry_reset(&policy);
    Check(policy.failures == 0 && hx_raw_retry_failed(&policy) == 200,
          "success/new lifecycle resets the backoff");
    for (unsigned bits = 0; bits < 64; ++bits) {
        Fixture f;
        f.ts.power = {bool(bits & 1), bool(bits & 2), bool(bits & 16),
                      bool(bits & 32), bool(bits & 4), bool(bits & 8)};
        const bool expected = bits == 7;
        hx_start_or_retry(&f.ts);
        Check(f.ts.detects == unsigned(expected) && f.ts.inits == unsigned(expected),
              "actual startup helper obeys all power/stream/initialization gates");
        if (!expected)
            Check(!f.ts.irq_changes && !f.ts.scheduled && f.ts.epoch == 7,
                  "duplicate work and closed gates leave IRQ, timers and epoch untouched");
    }
    for (bool detectFailure : {true, false}) {
        Fixture f;
        f.ts.detect_result = detectFailure ? -EIO : 0;
        f.ts.init_result = -ETIMEDOUT;
        for (unsigned i = 1; i <= 7; ++i) {
            hx_start_or_retry(&f.ts);
            Check(f.ts.detects == i && f.ts.inits == (detectFailure ? 0 : i),
                  "each work invocation attempts initialization once, not three immediate resets");
            Check(f.ts.scheduled == i && f.ts.work_kinds[i - 1] == 0 &&
                  f.ts.delays[i - 1] == hx_windows_backoff(i), "failure queues bounded Windows backoff");
            Check(!f.ts.power.controller_initialized && !f.ts.irq_enabled &&
                  f.ts.stopped_events == i && !f.ts.started_events && !f.ts.drains && !f.ts.sleeps,
                  "failure never publishes running, enables IRQ or sleeps for retry under mutex");
            Check(f.ts.epoch == 7 + i && !f.ts.sequence &&
                  f.ts.last_command_ns[HX_RAW_RESET] == 987654321 &&
                  f.ts.last_command_ns[HX_RAW_CALIBRATE] == 0,
                  "every attempt invalidates prior frames but preserves cross-epoch reset rate limit");
        }
        // A pending callback must not execute its scheduled reset after stop.
        for (unsigned gate : {0u, 1u, 2u, 3u}) {
            auto saved = f.ts.power;
            if (gate == 0) f.ts.power.streaming = false;
            if (gate == 1) f.ts.power.panel_enabled = false;
            if (gate == 2) f.ts.power.system_suspended = true;
            if (gate == 3) f.ts.power.shutting_down = true;
            const auto scheduled = f.ts.scheduled;
            hx_start_or_retry(&f.ts);
            Check(f.ts.detects == 7 && f.ts.scheduled == scheduled,
                  "queued retry rechecks STREAMOFF/panel/suspend/remove gates before hardware");
            f.ts.power = saved;
        }
        f.ts.detect_result = f.ts.init_result = 0;
        hx_start_or_retry(&f.ts);
        Check(f.ts.power.controller_initialized && f.ts.irq_enabled && f.ts.retry.failures == 0 &&
              f.ts.drains == 3 && f.ts.sleeps == 3 && f.ts.calibration == 1 && f.ts.rate == 1 &&
              f.ts.started_events == 1 && f.ts.read_errors == 0 && !f.ts.charger.applied_valid &&
              f.ts.work_kinds[7] == 1 && f.ts.delays[7] == 0,
              "eventual success clears failure state, drains, calibrates, starts IRQ and replays charger");
        const auto changes = f.ts.irq_changes;
        hx_start_or_retry(&f.ts);
        Check(f.ts.irq_changes == changes && f.ts.scheduled == 8, "late duplicate retry leaves healthy stream alone");
    }
    {
        Fixture f;
        f.ts.detect_result = -EIO;
        Check(hx_stream_on(&f.ts) == 0 && f.ts.power.streaming && !f.ts.power.controller_initialized &&
              f.ts.detects == 1 && f.ts.scheduled == 1 && f.ts.delays[0] == 200 &&
              f.ts.lifecycle_locks == 1 && f.ts.unlocks == 1 && !f.ts.returned_buffers,
              "STREAMON stays pending with one asynchronous retry, without queue failure/service restart");
        hx_stream_off(&f.ts);
        const auto epoch = f.ts.epoch;
        hx_start_or_retry(&f.ts);
        Check(!f.ts.power.streaming && f.ts.retry.failures == 0 && f.ts.epoch == epoch &&
              f.ts.detects == 1 && f.ts.scheduled == 1 && f.ts.returned_state == VB2_BUF_STATE_ERROR,
              "actual STREAMOFF clears backoff and blocks pending retry hardware access");
    }
    {
        Fixture f;
        f.ts.power.shutting_down = true;
        Check(hx_stream_on(&f.ts) == -ENODEV && f.ts.returned_buffers == 1 &&
              f.ts.returned_state == VB2_BUF_STATE_QUEUED && !f.ts.detects,
              "STREAMON on removed hardware remains a terminal failure");
        const auto locks = f.ts.lifecycle_locks;
        hx_stream_off(&f.ts);
        Check(f.ts.lifecycle_locks == locks && f.ts.mutex_locks == 1,
              "late STREAMOFF after shutdown does not touch already-released IRQ resources");
    }
    {
        Fixture f;
        f.ts.otp_tried = false;
        f.ts.calibration_result = f.ts.rate_result = f.ts.drain_result = -EIO;
        Check(hx_start_once(&f.ts) == 0 && f.ts.otps == 1 && f.ts.detects == 2 &&
              f.ts.drains == 3 && f.ts.sleeps == 3 && f.ts.calibration == 1 && f.ts.rate == 1 &&
              f.ts.power.controller_initialized,
              "OTP second reset and best-effort drain/calibration/rate policy are unchanged");
    }
    std::cout << "Actual AFE startup/retry: Windows backoff, one-attempt faults, all power gates, "
                 "late work, epoch/reset limits, eventual success and startup policy passed\n";
}
