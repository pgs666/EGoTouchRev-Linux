// SPDX-License-Identifier: MIT
#pragma once
#include "hx-afe.h"
#include "hx-v4l2-uapi.h"

#ifdef __cplusplus
extern "C" {
#endif
struct hx_dispatch_device {
    struct hx_afe_bus bus;
    u8 slot;
    int connected;
    int idle_retry;
};
int hx_dispatch_native(struct hx_dispatch_device *, u32, u32);
int hx_dispatch_windows(struct hx_dispatch_device *, u32, u8);
#ifdef __cplusplus
}
#endif
