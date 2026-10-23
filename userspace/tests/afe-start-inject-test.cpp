// SPDX-License-Identifier: MIT
#include "afe-start-host.h"
#include <cstdlib>
#include <iostream>

static void Check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

int main() {
    himax_ts_data ts{};
    ts.power = {true, true, false, false, true, false};
    ts.otp_tried = true;
    ts.panel_reinit_work = {&ts, 0};
    ts.charger_work = {&ts, 1};
    ts.op_lock = {&ts};
    ts.start_test_failures = 3;
    Check(hx_stream_on(&ts) == 0 && ts.start_test_failures == 2 &&
          !ts.detects && !ts.inits && !ts.drains && !ts.calibration && !ts.irq_enabled &&
          ts.retry.failures == 1 && ts.delays[0] == 200 && ts.epoch == 1,
          "diagnostic failure is stopped/pending before hardware, not a fabricated SPI error");
    hx_stream_off(&ts);
    hx_start_or_retry(&ts);
    Check(ts.start_test_failures == 2 && !ts.detects && ts.scheduled == 1 && ts.retry.failures == 0,
          "STREAMOFF prevents further injection/attempts from a late worker");
    Check(hx_stream_on(&ts) == 0 && ts.start_test_failures == 1 &&
          ts.retry.failures == 1 && ts.delays[1] == 200,
          "new stream restarts backoff without refreshing the per-device fault budget");
    hx_start_or_retry(&ts);
    Check(ts.start_test_failures == 0 && ts.retry.failures == 2 &&
          ts.delays[2] == 500 && !ts.detects && !ts.started_events,
          "last synthetic failure still cannot run initialization or report ready");
    hx_start_or_retry(&ts);
    Check(ts.detects == 1 && ts.inits == 1 && ts.drains == 3 && ts.calibration == 1 &&
          ts.rate == 1 && ts.irq_enabled && ts.started_events == 1 && ts.retry.failures == 0,
          "exhausted diagnostic budget reaches the unchanged real startup path");
    hx_stream_off(&ts);
    Check(hx_stream_on(&ts) == 0 && ts.detects == 2 && ts.retry.failures == 0 &&
          ts.start_test_failures == 0, "diagnostic budget never rearms on ordinary STREAMON");
    std::cout << "Diagnostic-only startup failures: no SPI, gate checks, per-device budget, "
                 "STREAMOFF/new stream and eventual real startup passed\n";
}
