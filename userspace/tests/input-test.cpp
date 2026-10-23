// SPDX-License-Identifier: MIT
#include "input.h"
#include <cstdlib>
#include <iostream>

static void Check(bool value, const char* name) {
    if (!value) { std::cerr << "FAIL: " << name << '\n'; std::exit(1); }
}

static int Events(const himax::SlotState& slots, int code, int value) {
    int count = 0;
    for (const auto& event : slots.Events())
        if (event.type == EV_ABS && event.code == code && event.value == value) ++count;
    return count;
}

int main() {
    himax::SlotState slots;
    himax::Contact points[] = {{31, 100, 100, 200}, {4, 200, 300, 300}};
    constexpr uint64_t sampleNs = 1234567890123;
    Check(slots.Frame(points, sampleNs) && slots.Active(), "two sparse IDs down");
    Check(slots.Events().front().type == EV_KEY && slots.Events().front().code == BTN_TOUCH,
          "touch key precedes coordinates");
    for (const auto& event : slots.Events())
        Check(event.input_event_sec == 1234 && event.input_event_usec == 567890,
              "all events retain the acquisition timestamp");
    Check(Events(slots, ABS_MT_TRACKING_ID, 31) == 1, "ID 31 supported");
    Check(slots.Frame(points, sampleNs - 1000) &&
          slots.Events().back().input_event_usec == 567890, "input time cannot go backwards");
    Check(Events(slots, ABS_MT_TRACKING_ID, 31) == 0, "move preserves tracking ID");
    points[0].id = 7;
    Check(slots.Frame(points), "ID replacement");
    Check(Events(slots, ABS_MT_TRACKING_ID, -1) == 1, "old ID released once");
    Check(Events(slots, ABS_MT_TRACKING_ID, 7) == 1, "new ID starts once");
    points[0].id = 4;
    Check(!slots.Frame(points), "duplicate rejected before any change");
    Check(slots.Frame({}) && !slots.Active(), "screen off releases all");
    Check(Events(slots, ABS_MT_TRACKING_ID, -1) == 2, "all slots released");
    Check(slots.Frame({}) && Events(slots, ABS_MT_TRACKING_ID, -1) == 0, "release is idempotent");
    std::array<himax::Contact, 10> full{};
    for (size_t i = 0; i < full.size(); ++i) full[i].id = i;
    Check(slots.Frame(full), "ten contacts accepted");
    for (auto& point : full) point.id += 10;
    Check(slots.Frame(full), "all ten IDs replaced in one frame");
    Check(Events(slots, ABS_MT_TRACKING_ID, -1) == 10, "ten old IDs released");
    // All eight firmware wiring combinations, independently specified for
    // the original top-left corner; the opposite corner must complement it.
    constexpr int corners[8][2] = {
        {0, 0}, {25599, 0}, {0, 15999}, {25599, 15999},
        {0, 0}, {0, 25599}, {15999, 0}, {15999, 25599},
    };
    for (uint32_t flags = 0; flags < 8; ++flags) {
        himax::SlotState oriented(flags);
        himax::Contact corner{1, 0, 0, 123};
        Check(oriented.Frame(std::span(&corner, 1)), "oriented corner accepted");
        Check(Events(oriented, ABS_MT_POSITION_X, corners[flags][0]) == 1 &&
              Events(oriented, ABS_MT_POSITION_Y, corners[flags][1]) == 1 &&
              Events(oriented, ABS_X, corners[flags][0]) == 1 &&
              Events(oriented, ABS_Y, corners[flags][1]) == 1,
              "firmware inversion precedes swap on MT and legacy axes");
        corner.x = 25599;
        corner.y = 15999;
        Check(oriented.Frame(std::span(&corner, 1)) &&
              Events(oriented, ABS_MT_POSITION_X, oriented.XMax() - corners[flags][0]) == 1 &&
              Events(oriented, ABS_MT_POSITION_Y, oriented.YMax() - corners[flags][1]) == 1,
              "opposite corner and declared axis ranges agree");
    }
    himax::SlotState gaokun(HX_RAW_INVERT_X | HX_RAW_SWAP_XY);
    const himax::Contact moving{1, 100, 200, 123};
    Check(gaokun.Frame(std::span(&moving, 1)) &&
          Events(gaokun, ABS_MT_POSITION_X, 200) == 1 &&
          Events(gaokun, ABS_MT_POSITION_Y, 25499) == 1,
          "Gaokun interior point matches firmware wiring");
    himax::SlotState unsupported(8);
    Check(!unsupported.Frame({}), "unknown firmware transform rejected");
    std::cout << "Type-B slot lifecycle passed\n";
}
