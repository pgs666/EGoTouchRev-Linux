// SPDX-License-Identifier: MIT
#pragma once
#include "hx-raw-policy.h"
#include "hx-v4l2-uapi.h"
#include <errno.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif
struct himax_ts_data;
struct delayed_work { struct himax_ts_data *owner; unsigned kind; };
struct mutex { struct himax_ts_data *owner; };
struct vb2_queue { void *drv_priv; };
enum vb2_buffer_state { VB2_BUF_STATE_QUEUED, VB2_BUF_STATE_ERROR };
struct himax_ts_data {
    void *dev;
    struct hx_raw_power_state power;
    struct hx_raw_retry_state retry;
    struct hx_raw_charger_state charger;
    uint64_t epoch, sequence, last_command_ns[HX_RAW_RESET + 1];
    char project_id[16];
    bool otp_tried, irq_enabled;
    unsigned char afe_command_slot, read_errors;
    struct delayed_work panel_reinit_work, charger_work;
    struct mutex op_lock;
    uint32_t frame_flags;
    int detect_result, init_result, calibration_result, rate_result, drain_result;
    unsigned detects, otps, inits, drains, calibration, rate, sleeps, irq_changes;
    unsigned stopped_events, started_events, scheduled;
    unsigned work_kinds[64], delays[64];
    unsigned lifecycle_locks, mutex_locks, unlocks, returned_buffers, returned_state;
};
int hx_start_once(struct himax_ts_data *);
void hx_start_or_retry(struct himax_ts_data *);
int hx_stream_on(struct himax_ts_data *);
void hx_stream_off(struct himax_ts_data *);
unsigned hx_windows_backoff(int failures);

static inline void *vb2_get_drv_priv(struct vb2_queue *q) { return q->drv_priv; }
static inline void himax_lock(struct himax_ts_data *ts) { ++ts->lifecycle_locks; }
static inline void himax_unlock(struct himax_ts_data *ts) { ++ts->unlocks; }
static inline void mutex_lock(struct mutex *m) { ++m->owner->mutex_locks; }
static inline void mutex_unlock(struct mutex *m) { ++m->owner->unlocks; }
static inline void himax_return_buffers(struct himax_ts_data *ts, enum vb2_buffer_state state) {
    ++ts->returned_buffers;
    ts->returned_state = state;
}

static inline int hx83121a_chip_detect(struct himax_ts_data *ts) {
    ++ts->detects;
    return ts->detect_result;
}
static inline int himax_get_project_id_otp(struct himax_ts_data *ts, char *id, size_t size) {
    (void)size;
    ++ts->otps;
    memcpy(id, "W273AS2700", 11);
    return 0;
}
static inline int himax_mcu_power_on_init(struct himax_ts_data *ts) {
    ++ts->inits;
    return ts->init_result;
}
static inline int himax_read_raw(struct himax_ts_data *ts) {
    ++ts->drains;
    return ts->drain_result;
}
static inline int himax_afe_command(struct himax_ts_data *ts, unsigned action, unsigned parameter) {
    if (parameter) return -EINVAL;
    if (action == HX_RAW_CALIBRATE) { ++ts->calibration; return ts->calibration_result; }
    if (action == HX_RAW_FORCE_RATE) { ++ts->rate; return ts->rate_result; }
    return -EINVAL;
}
static inline void himax_state_event(struct himax_ts_data *ts, bool running) {
    if (running) ++ts->started_events;
    else ++ts->stopped_events;
}
static inline void himax_int_enable(struct himax_ts_data *ts, bool enable) {
    ++ts->irq_changes;
    ts->irq_enabled = enable;
}
static inline bool mod_delayed_work(void *queue, struct delayed_work *work, unsigned delay) {
    struct himax_ts_data *ts = work->owner;
    (void)queue;
    if (ts->scheduled < 64) {
        ts->work_kinds[ts->scheduled] = work->kind;
        ts->delays[ts->scheduled] = delay;
    }
    ++ts->scheduled;
    return false;
}
#define system_wq ((void *)0)
#define msecs_to_jiffies(value) (value)
// Protocol waits are tested separately. No wall-time sleeps or real IRQ/SPI.
#define usleep_range(low, high) (++ts->sleeps)
#define dev_err(...) ((void)0)
#define dev_warn(...) ((void)0)
#define dev_warn_ratelimited(...) ((void)0)
#ifdef __cplusplus
}
#endif
