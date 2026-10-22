// SPDX-License-Identifier: MIT
#include "stream-state.h"
#include <cstdlib>
#include <iostream>

static void Check(bool value, const char* name) {
    if (!value) { std::cerr << "FAIL: " << name << '\n'; std::exit(1); }
}

int main() {
    static_assert(sizeof(hx_raw_frame) == 5184);
    static_assert(sizeof(hx_raw_info) == 40);
    static_assert(sizeof(hx_raw_command) == 40);
    static_assert(sizeof(hx_raw_state) == 16);
    static_assert(offsetof(hx_raw_frame, payload) == 48);
    himax::StreamState stream;
    hx_raw_frame frame{};
    frame.epoch = 1;
    frame.sequence = 1;
    frame.timestamp_ns = 1000000000;
    Check(stream.Accept(frame, frame.timestamp_ns), "first frame starts stream");
    Check(!stream.Accept(frame, frame.timestamp_ns), "duplicate rejected");
    Check(stream.State({HX_RAW_ABI_VERSION, 0, 2}), "stop accepted");
    ++frame.sequence;
    Check(!stream.Accept(frame, frame.timestamp_ns), "old queued frame after screen off rejected");
    frame.epoch = 2;
    Check(!stream.Accept(frame, frame.timestamp_ns), "stopped epoch cannot restart from queued frame");
    Check(stream.State({HX_RAW_ABI_VERSION, 1, 3}), "wake accepted");
    frame.epoch = 3;
    frame.sequence = 1;
    Check(stream.Accept(frame, frame.timestamp_ns), "new epoch resets sequence");
    Check(!stream.State({HX_RAW_ABI_VERSION, 0, 2}), "late old stop ignored");
    ++frame.sequence;
    Check(!stream.Accept(frame, frame.timestamp_ns + 50000001), "stale frame rejected");
    Check(!stream.Accept(frame, frame.timestamp_ns - 1), "future frame rejected");
    Check(stream.Accept(frame, frame.timestamp_ns), "fresh frame after ignored stop accepted");
    frame.sequence = 1;
    Check(!stream.Accept(frame, frame.timestamp_ns), "out of order sequence rejected");
    frame.epoch = 4;
    Check(stream.Accept(frame, frame.timestamp_ns), "epoch advances even if event was dropped");
    std::cout << "Stream epoch, freshness and ABI tests passed\n";
}
