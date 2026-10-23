// SPDX-License-Identifier: MIT
#pragma once
#include "hx-v4l2-uapi.h"
#include "hx-raw-policy.h"
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef uint64_t u64;
typedef int irqreturn_t;
#define IRQ_HANDLED 1
#define NSEC_PER_MSEC 1000000ULL
#define V4L2_FIELD_NONE 0
#define VB2_BUF_STATE_DONE 1
#define system_wq ((void *)0)
struct list_head { struct list_head *next, *prev; };
static inline bool list_empty(const struct list_head *h) { return h->next == h; }
static inline void list_del(struct list_head *h) { h->prev->next = h->next; h->next->prev = h->prev; }
#define list_first_entry(head, type, member) ((type *)((char *)(head)->next - offsetof(type, member)))
#define spin_lock_irqsave(lock, flags) ((void)(lock), (flags) = 0)
#define spin_unlock_irqrestore(lock, flags) ((void)(lock), (void)(flags))
struct vb2_buffer { u64 timestamp; struct hx_raw_frame frame; unsigned done, bytes; };
struct vb2_v4l2_buffer { struct vb2_buffer vb2_buf; u64 sequence; unsigned field; };
struct vb2_queue { void *owner; bool busy; };
struct file { void *private_data; };
struct himax_ts_data {
    pthread_mutex_t op_lock;
    struct hx_raw_power_state power;
    struct hx_raw_charger_state charger;
    struct vb2_queue queue;
    struct list_head buffers;
    int buffer_lock;
    u64 epoch, sequence, last_frame_ns, last_rearm_ns;
    unsigned read_errors, frame_flags;
    unsigned char event_buf[HX_RAW_PAYLOAD_SIZE];
    int spi_error;
    unsigned reads, schedules, state_events, irq_changes;
    int panel_reinit_work;
};
extern u64 hx_test_now;
// Called before acquiring the real pthread mutex; used to arrange actual
// IRQ/rearm ordering without copying the production validation logic.
extern void (*hx_before_lock)(void);
static inline u64 ktime_get_ns(void) { return hx_test_now; }
static inline void mutex_lock(pthread_mutex_t *m) {
    if (hx_before_lock) hx_before_lock();
    pthread_mutex_lock(m);
}
static inline void mutex_unlock(pthread_mutex_t *m) { pthread_mutex_unlock(m); }
static inline int himax_read_raw(struct himax_ts_data *ts) {
    ++ts->reads;
    if (ts->spi_error) return ts->spi_error;
    memset(ts->event_buf, 0x37, HX_RAW_MASTER_SIZE);
    memset(ts->event_buf + HX_RAW_MASTER_SIZE, 0, HX_RAW_PAYLOAD_SIZE - HX_RAW_MASTER_SIZE);
    return 0;
}
static inline void *vb2_plane_vaddr(struct vb2_buffer *buf, unsigned plane) { (void)plane; return &buf->frame; }
static inline void vb2_set_plane_payload(struct vb2_buffer *buf, unsigned plane, unsigned bytes) {
    (void)plane; buf->bytes = bytes;
}
static inline void vb2_buffer_done(struct vb2_buffer *buf, unsigned state) { buf->done = state; }
static inline bool vb2_is_busy(struct vb2_queue *q) { return q->busy; }
static inline void himax_int_enable(struct himax_ts_data *ts, bool on) { (void)on; ++ts->irq_changes; }
static inline void himax_state_event(struct himax_ts_data *ts, bool on) { (void)on; ++ts->state_events; }
static inline bool mod_delayed_work(void *wq, int *work, unsigned delay) {
    (void)wq; (void)delay;
    struct himax_ts_data *ts = (struct himax_ts_data *)((char *)work - offsetof(struct himax_ts_data, panel_reinit_work));
    ++ts->schedules;
    return false;
}
int hx_rearm(struct himax_ts_data *, struct file *, const struct hx_raw_rearm *);
int hx_irq(struct himax_ts_data *);
void hx_enqueue(struct himax_ts_data *, void *);
unsigned long hx_buffer_size(void);
#ifdef __cplusplus
}
#endif
