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
    Check(slots.Frame(points) && slots.Active(), "two sparse IDs down");
    Check(Events(slots, ABS_MT_TRACKING_ID, 31) == 1, "ID 31 supported");
    Check(slots.Frame(points), "move");
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
    std::cout << "Type-B slot lifecycle passed\n";
}
