// SPDX-License-Identifier: MIT
#include "rearm-host.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

u64 hx_test_now = 2000000000;
void (*hx_before_lock)(void) = nullptr;
static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
struct Fixture {
    himax_ts_data ts{};
    file owner{this};
    hx_raw_rearm request{sizeof(request), HX_RAW_ABI_VERSION, 7, 0, {0, 0}};
    void* buffer = std::calloc(1, hx_buffer_size());
    Fixture() {
        Check(buffer != nullptr, "allocation");
        pthread_mutex_init(&ts.op_lock, nullptr);
        ts.power = {true, true, false, false, true, true};
        ts.queue = {this, true};
        ts.epoch = 7;
        ts.last_frame_ns = 1000000000;
        ts.buffers.next = ts.buffers.prev = &ts.buffers;
        hx_enqueue(&ts, buffer);
        hx_test_now = 2000000000;
    }
    ~Fixture() { pthread_mutex_destroy(&ts.op_lock); std::free(buffer); }
    int Rearm() { return hx_rearm(&ts, &owner, &request); }
    const vb2_v4l2_buffer& Output() const { return *static_cast<vb2_v4l2_buffer*>(buffer); }
};

static std::atomic<bool> irqWaiting{false}, allowIrq{false};
static void BeforeLock() {
    irqWaiting.store(true, std::memory_order_release);
    while (!allowIrq.load(std::memory_order_acquire)) std::this_thread::yield();
}

int main() {
    {
        Fixture f;
        f.ts.frame_flags = HX_RAW_FRAME_SOS;
        f.ts.charger.connected = true;
        Check(f.Rearm() == 0 && f.ts.reads == 1 && f.ts.sequence == 1, "missing first IRQ rearmed");
        const auto& vb = f.Output();
        Check(vb.vb2_buf.done == VB2_BUF_STATE_DONE && vb.vb2_buf.bytes == sizeof(hx_raw_frame), "one complete buffer published");
        const auto& p = vb.vb2_buf.frame;
        Check(p.size == sizeof(p) && p.version == HX_RAW_ABI_VERSION && p.epoch == 7 &&
              p.sequence == 1 && p.timestamp_ns == hx_test_now && p.payload_size == HX_RAW_MASTER_SIZE,
              "normal HXST metadata preserved");
        Check(p.flags == (HX_RAW_FRAME_SOS | HX_RAW_FRAME_CHARGER), "flags match normal IRQ publication");
        Check(p.reserved[0] == 0 && p.reserved[1] == 0 && p.reserved_tail == 0 &&
              p.payload[HX_RAW_MASTER_SIZE - 1] == 0x37 && p.payload[HX_RAW_MASTER_SIZE] == 0,
              "full payload and zero tail");
        Check(f.ts.epoch == 7 && f.ts.power.controller_initialized && f.ts.schedules == 0 &&
              f.ts.state_events == 0 && f.ts.irq_changes == 0, "rearm never resets AFE or publishes lifecycle changes");
        Check(f.Rearm() == -ESTALE && f.ts.reads == 1, "same token cannot be reused");
    }
    for (int field = 0; field < 4; ++field) {
        Fixture f;
        if (field == 0) --f.request.size;
        if (field == 1) ++f.request.version;
        if (field == 2) f.request.reserved[0] = 1;
        if (field == 3) f.request.reserved[1] = 1;
        Check(f.Rearm() == -EINVAL && f.ts.reads == 0, "malformed request has no I/O");
    }
    for (int gate = 0; gate < 8; ++gate) {
        Fixture f;
        if (gate == 0) f.ts.power.panel_prepared = false;
        if (gate == 1) f.ts.power.panel_enabled = false;
        if (gate == 2) f.ts.power.system_suspended = true;
        if (gate == 3) f.ts.power.streaming = false;
        if (gate == 4) f.ts.power.controller_initialized = false;
        if (gate == 5) f.request.epoch = 0;
        if (gate == 6) f.request.epoch++;
        if (gate == 7) f.request.sequence++;
        Check(f.Rearm() == -ESTALE && f.ts.reads == 0, "lifecycle/epoch/sequence gates");
        if (gate < 5) {
            hx_irq(&f.ts);
            Check(f.ts.reads == 0, "IRQ obeys the same lifecycle gates");
        }
    }
    {
        Fixture f;
        f.ts.power.shutting_down = true;
        Check(f.Rearm() == -ENODEV && f.ts.reads == 0, "shutdown gate");
    }
    for (bool busy : {false, true}) {
        Fixture f;
        f.ts.queue.busy = busy;
        if (busy) f.owner.private_data = nullptr;
        Check(f.Rearm() == -EBUSY && f.ts.reads == 0, "queue and owner gate");
    }
    {
        Fixture f;
        hx_test_now = f.ts.last_frame_ns + 199999999;
        Check(f.Rearm() == -EAGAIN && f.ts.reads == 0, "recent producer cannot rearm");
        hx_test_now++;
        Check(f.Rearm() == 0, "quiet deadline inclusive");
    }
    {
        Fixture f;
        f.ts.buffers.next = f.ts.buffers.prev = &f.ts.buffers;
        Check(f.Rearm() == -ENOBUFS && f.ts.reads == 0, "backpressure does not consume a frame");
        hx_irq(&f.ts);
        Check(f.ts.sequence == 1 && f.ts.reads == 1, "normal IRQ still drains without available buffer");
    }
    for (int error : {-EIO, -ESHUTDOWN, -EMSGSIZE}) {
        Fixture f;
        f.ts.spi_error = error;
        std::fill(std::begin(f.ts.event_buf), std::end(f.ts.event_buf), 0x96);
        for (unsigned i = 0; i < 8; ++i) {
            Check(f.Rearm() == error && f.ts.sequence == 0 && f.Output().vb2_buf.done == 0,
                  "SPI failure cannot publish or advance sequence");
            Check(f.Rearm() == -EAGAIN, "failure attempts rate limited");
            hx_test_now += 200000000;
        }
        Check(f.ts.schedules == 0 && f.ts.state_events == 0 && f.ts.power.controller_initialized,
              "rearm failures alone cannot reset AFE");
        Check(f.ts.event_buf[0] == 0x96 && f.ts.event_buf[HX_RAW_PAYLOAD_SIZE - 1] == 0x96,
              "last complete evidence preserved by read contract");
    }
    {
        Fixture f;
        hx_irq(&f.ts);
        Check(f.Rearm() == -ESTALE && f.ts.reads == 1, "IRQ wins: stale kick does not read again");
    }
    {
        Fixture f;
        f.ts.spi_error = -EIO;
        f.ts.read_errors = 2;
        Check(f.Rearm() == -EIO && f.ts.read_errors == 2 && f.ts.schedules == 0,
              "kick failure cannot consume IRQ recovery budget");
        hx_irq(&f.ts);
        Check(f.ts.read_errors == 3 && f.ts.schedules == 1 && f.ts.state_events == 1 &&
              !f.ts.power.controller_initialized, "existing third-IRQ failure recovery preserved");
    }
    {
        Fixture f;
        // Hold the real mutex while an IRQ timestamps its invocation and
        // reaches the lock. Publish a later kick before releasing that IRQ.
        pthread_mutex_lock(&f.ts.op_lock);
        hx_before_lock = BeforeLock;
        std::thread irq([&] { hx_irq(&f.ts); });
        while (!irqWaiting.load(std::memory_order_acquire)) std::this_thread::yield();
        hx_before_lock = nullptr;
        ++hx_test_now;
        pthread_mutex_unlock(&f.ts.op_lock);
        Check(f.Rearm() == 0, "kick wins against an already timestamped IRQ");
        allowIrq.store(true, std::memory_order_release);
        irq.join();
        Check(f.ts.reads == 1 && f.ts.sequence == 1, "older pending IRQ cannot reread kicked frame");
    }
    std::cout << "Production rearm/IRQ capture, owner/lifecycle/rate gates, failures and lock ordering passed\n";
}
