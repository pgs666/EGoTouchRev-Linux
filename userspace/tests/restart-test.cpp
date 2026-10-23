// SPDX-License-Identifier: MIT
// Compile the production restart/publication/IRQ bodies, replacing OS and SPI.
#include "rearm-host.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

u64 hx_test_now = 2000000000;
void (*hx_before_lock)(void) = nullptr;
void (*hx_during_read)(void) = nullptr;

static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

struct Fixture {
    himax_ts_data ts{};
    file owner{this};
    hx_raw_restart request{sizeof(request), HX_RAW_ABI_VERSION, 7, 0, {0, 0}};
    Fixture() {
        pthread_mutex_init(&ts.op_lock, nullptr);
        ts.power = {true, true, false, false, true, true};
        ts.queue = {this, true};
        ts.panel_reinit_work = {&ts, 0};
        ts.charger_work = {&ts, 1};
        ts.epoch = 7;
        ts.last_frame_ns = 1000000000;
        ts.buffers.next = ts.buffers.prev = &ts.buffers;
        ts.irq_enabled = true;
        ts.last_state_running = true;
        std::memset(ts.event_buf, 0x96, sizeof(ts.event_buf));
        hx_test_now = 2000000000;
    }
    ~Fixture() { pthread_mutex_destroy(&ts.op_lock); }
    int Restart() { return hx_restart(&ts, &owner, &request); }
    void NoEffects() const {
        Check(!ts.reads && !ts.schedules && !ts.state_events && !ts.irq_changes &&
              !ts.last_command_ns[HX_RAW_RESET] && ts.irq_enabled &&
              ts.last_state_running && ts.event_buf[0] == 0x96 &&
              ts.event_buf[HX_RAW_PAYLOAD_SIZE - 1] == 0x96,
              "rejected request has no SPI, scheduling, event, IRQ or limiter effects");
    }
};

static std::atomic<bool> waiting{false}, proceed{false};
static void BeforeLock() {
    waiting.store(true, std::memory_order_release);
    while (!proceed.load(std::memory_order_acquire)) std::this_thread::yield();
}
static void WaitAtLock() {
    waiting.store(false, std::memory_order_relaxed);
    proceed.store(false, std::memory_order_relaxed);
    hx_before_lock = BeforeLock;
}
static void WaitForThread() {
    while (!waiting.load(std::memory_order_acquire)) std::this_thread::yield();
    hx_before_lock = nullptr;
}

int main() {
    static_assert(sizeof(hx_raw_restart) == 32);
    static_assert(alignof(hx_raw_restart) == 8);
    static_assert(offsetof(hx_raw_restart, epoch) == 8 &&
                  offsetof(hx_raw_restart, sequence) == 16 &&
                  offsetof(hx_raw_restart, reserved) == 24);
    static_assert(VIDIOC_HIMAX_RESTART != VIDIOC_HIMAX_REARM &&
                  VIDIOC_HIMAX_RESTART != VIDIOC_HIMAX_REARM_TAGGED &&
                  VIDIOC_HIMAX_RESTART != VIDIOC_HIMAX_COMMAND);
    {
        Fixture f;
        Check(f.Restart() == 0, "stalled epoch with no first frame and no free buffer can restart");
        Check(!f.ts.reads && f.ts.schedules == 1 && f.ts.state_events == 1 &&
              f.ts.irq_changes == 1 && !f.ts.irq_enabled && !f.ts.last_state_running &&
              !f.ts.power.controller_initialized && f.ts.frame_flags == HX_RAW_FRAME_SOS &&
              f.ts.last_command_ns[HX_RAW_RESET] == hx_test_now,
              "accepted means stopped event and scheduled client reconstruction, not recovered");
        Check(f.ts.epoch == 7 && !f.ts.sequence && f.ts.event_buf[0] == 0x96 &&
              f.ts.event_buf[HX_RAW_PAYLOAD_SIZE - 1] == 0x96,
              "request does not invent epoch/frame or erase last complete evidence");
        Check(f.Restart() == -ESTALE && f.ts.schedules == 1 && f.ts.state_events == 1,
              "pending reconstruction closes read gate and cannot schedule twice");
        hx_irq(&f.ts);
        Check(!f.ts.reads && !f.ts.sequence, "IRQ after restart admission cannot touch SPI");
    }
    for (int field = 0; field < 4; ++field) {
        Fixture f;
        if (field == 0) --f.request.size;
        if (field == 1) ++f.request.version;
        if (field == 2) f.request.reserved[0] = 1;
        if (field == 3) f.request.reserved[1] = 1;
        Check(f.Restart() == -EINVAL, "strict request layout and version");
        f.NoEffects();
    }
    for (int gate = 0; gate < 11; ++gate) {
        Fixture f;
        if (gate == 0) f.ts.power.panel_prepared = false;
        if (gate == 1) f.ts.power.panel_enabled = false;
        if (gate == 2) f.ts.power.system_suspended = true;
        if (gate == 3) f.ts.power.streaming = false;
        if (gate == 4) f.ts.power.controller_initialized = false;
        if (gate == 5) f.ts.startup.active = true;
        if (gate == 6) f.request.epoch = 0;
        if (gate == 7) --f.request.epoch;
        if (gate == 8) ++f.request.epoch;
        if (gate == 9) { f.ts.sequence = 8; f.request.sequence = 7; }
        if (gate == 10) { f.ts.sequence = 8; f.request.sequence = 9; }
        Check(f.Restart() == -ESTALE, "power, startup, exact nonzero epoch and exact sequence gates");
        f.NoEffects();
    }
    {
        Fixture f;
        f.ts.power.shutting_down = true;
        Check(f.Restart() == -ENODEV, "shutdown is a disconnected device");
        f.NoEffects();
    }
    for (bool allocated : {false, true}) {
        Fixture f;
        f.ts.queue.busy = allocated;
        if (allocated) f.owner.private_data = nullptr;
        Check(f.Restart() == -EBUSY, "allocated queue must belong to this file handle");
        f.NoEffects();
    }
    for (bool poll : {false, true}) {
        Fixture f;
        (poll ? f.ts.last_poll_done_ns : f.ts.last_frame_ns) = hx_test_now - 199999999;
        Check(f.Restart() == -EAGAIN, "199999999ns silence is not rounded up");
        f.NoEffects();
        ++hx_test_now;
        Check(f.Restart() == 0, "full 200ms quiet since acquisition AND poll completion");
    }
    for (bool legacyFirst : {false, true}) {
        Fixture f;
        const auto firstTime = hx_test_now;
        // Same production helper as the already-validated legacy RESET path.
        Check((legacyFirst ? hx_schedule_restart(&f.ts) : f.Restart()) == 0,
              "first reset/restart request admitted");
        // Model a new ready epoch without granting a fresh reset budget.
        ++f.ts.epoch;
        f.request.epoch = f.ts.epoch;
        f.ts.power.controller_initialized = true;
        hx_test_now = firstTime + NSEC_PER_SEC - 1;
        Check((legacyFirst ? f.Restart() : hx_schedule_restart(&f.ts)) == -EAGAIN &&
              f.ts.schedules == 1 && f.ts.last_command_ns[HX_RAW_RESET] == firstTime,
              "legacy RESET and stalled restart share one cross-epoch limiter");
        ++hx_test_now;
        Check((legacyFirst ? f.Restart() : hx_schedule_restart(&f.ts)) == 0 &&
              f.ts.schedules == 2, "exact one-second deadline permits next reset");
    }
    {
        Fixture f;
        f.ts.sequence = f.request.sequence = 123;
        Check(f.Restart() == 0 && f.ts.sequence == 123,
              "exact nonzero sequence accepted without publishing another frame");
    }
    {
        Fixture f;
        void* buffer = std::calloc(1, hx_buffer_size());
        Check(buffer != nullptr, "retained evidence buffer allocation");
        hx_enqueue(&f.ts, buffer);
        hx_irq(&f.ts);
        auto& output = static_cast<vb2_v4l2_buffer*>(buffer)->vb2_buf;
        const auto saved = output.frame;
        f.request.sequence = f.ts.sequence;
        hx_test_now += 200000000;
        Check(f.Restart() == 0 && f.ts.reads == 1 && f.ts.sequence == 1 &&
              output.done == VB2_BUF_STATE_DONE &&
              !std::memcmp(&saved, &output.frame, sizeof(saved)),
              "scheduling preserves already-completed old-epoch evidence without reading or relabeling it");
        std::free(buffer);
    }
    {
        Fixture f;
        int result = 1;
        WaitAtLock();
        std::thread requester([&] { result = f.Restart(); });
        WaitForThread();
        hx_irq(&f.ts); // Takes real mutex and advances the producer.
        hx_test_now += 200000000;
        proceed.store(true, std::memory_order_release);
        requester.join();
        Check(result == -ESTALE && f.ts.reads == 1 && f.ts.sequence == 1 &&
              !f.ts.schedules && !f.ts.state_events && f.ts.power.controller_initialized,
              "IRQ wins lock: even a now-quiet old restart token cannot reset fresh producer");
    }
    {
        Fixture f;
        WaitAtLock();
        std::thread irq([&] { hx_irq(&f.ts); });
        WaitForThread();
        Check(f.Restart() == 0, "restart wins against timestamped pending IRQ");
        proceed.store(true, std::memory_order_release);
        irq.join();
        Check(!f.ts.reads && !f.ts.sequence && f.ts.schedules == 1 &&
              !f.ts.power.controller_initialized, "losing IRQ observes closed gate and cannot read");
    }
    for (bool suspended : {false, true}) {
        Fixture f;
        int result = 1;
        WaitAtLock();
        std::thread requester([&] { result = f.Restart(); });
        WaitForThread();
        pthread_mutex_lock(&f.ts.op_lock);
        if (suspended) f.ts.power.system_suspended = true;
        else f.ts.power.streaming = false;
        pthread_mutex_unlock(&f.ts.op_lock);
        proceed.store(true, std::memory_order_release);
        requester.join();
        Check(result == -ESTALE, "suspend/STREAMOFF wins lock: pending restart rejected");
        f.NoEffects();
    }
    std::cout << "Production stalled restart layout/owner/power/token/quiet/shared-rate gates and IRQ races passed\n";
}
