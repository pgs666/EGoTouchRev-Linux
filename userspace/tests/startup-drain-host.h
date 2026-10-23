// SPDX-License-Identifier: MIT
#pragma once

enum hx_startup_drain_operation {
    HX_DRAIN_OPEN, HX_DRAIN_CLOSE, HX_DRAIN_VALID,
    HX_DRAIN_BLOCK, HX_DRAIN_TIMEOUT, HX_DRAIN_FRAME,
};
struct hx_startup_drain_bus {
    void* context;
    int (*operation)(void*, unsigned channel, unsigned operation, unsigned value);
};

extern "C" int hx_windows_startup_drain(hx_startup_drain_bus*, unsigned* discarded);
