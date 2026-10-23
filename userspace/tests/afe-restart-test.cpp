// SPDX-License-Identifier: MIT
#include "afe-restart.h"
#include <cstdlib>
#include <iostream>

static void Check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

int main() {
    using Policy = himax::AfeRestart;
    using Step = Policy::Step;
    constexpr uint64_t base = 1000000000, ms = 1000000;
    const auto Running = [](uint64_t epoch, uint64_t now) {
        himax::StreamState stream;
        stream.State({HX_RAW_ABI_VERSION, 1, epoch}, now);
        return stream;
    };
    hx_raw_frame packet{};
    packet.epoch = 2;
    packet.sequence = 1;
    for (int error : {ENOTTY, EIO, ENODEV, EINVAL, ENOBUFS}) {
        auto stream = Running(1, base);
        Policy policy;
        Check(policy.Next(stream, base, false) == Step::None, "healthy stream cannot request restart");
        Check(policy.Next(stream, base + 4001 * ms, true) == Step::Request &&
              policy.Epoch() == 1 && policy.Sequence() == 0,
              "exhausted producer with no first packet has precise request token");
        Check(!policy.Result(error, base + 4002 * ms) && !policy.Waiting(),
              "rejected ioctl neither stops solver nor claims recovery");
        Check(policy.Next(stream, base + 4003 * ms, true) == Step::Fallback,
              "unsupported/fatal device errors preserve supervisor fallback");
    }
    for (int error : {ESTALE, EAGAIN, EBUSY}) {
        auto stream = Running(1, base);
        Policy policy;
        auto now = base + 4001 * ms;
        Check(policy.Next(stream, now, true) == Step::Request, "first transient request");
        Check(!policy.Result(error, now + ms), "race cannot manufacture acceptance");
        Check(policy.Next(stream, now + 100 * ms, true) == Step::None,
              "retry grace measured from ioctl completion");
        Check(policy.Next(stream, now + 101 * ms, true) == Step::Request, "100ms transient backoff");
        policy.Result(error, now + 102 * ms);
        Check(policy.Next(stream, now + Policy::kRaceBudgetNs, true) == Step::Fallback,
              "per-stall transient failures bounded to one second");
    }
    {
        auto stream = Running(1, base);
        Policy policy;
        Check(policy.Next(stream, base + 5000 * ms, true) == Step::Request, "race fixture");
        policy.Result(ESTALE, base + 5001 * ms);
        packet.epoch = 1;
        packet.timestamp_ns = base + 5002 * ms;
        Check(stream.Accept(packet, packet.timestamp_ns), "winning IRQ delivered");
        Check(policy.Next(stream, base + 5200 * ms, true) == Step::None,
              "changed producer progress cancels old token before another request");
        Check(policy.Next(stream, base + 5201 * ms, false) == Step::None,
              "new healthy producer has no pending recovery");
        stream.State({HX_RAW_ABI_VERSION, 0, 2}, base + 5300 * ms);
        Check(policy.Next(stream, base + 9000 * ms, true) == Step::None,
              "stopped/panel-off stream cannot initiate reconstruction");
    }
    {
        auto stream = Running(1, base);
        Policy policy;
        auto now = base + 5000 * ms;
        Check(policy.Next(stream, now, true) == Step::Request && policy.Result(0, now + ms),
              "accepted schedules reconstruction");
        Check(policy.Waiting(), "acceptance is not data recovery");
        packet.epoch = 1;
        Check(!policy.Allows(packet) && !policy.Allows(hx_raw_state{HX_RAW_ABI_VERSION, 1, 1}),
              "late old packet and late old ready event cannot reopen solved epoch");
        Check(policy.Allows(hx_raw_state{HX_RAW_ABI_VERSION, 0, 1}),
              "matching stopped lifecycle still accepted");
        packet.epoch = 2;
        Check(policy.Allows(packet), "new epoch data may undergo validity checks");
        Check(!policy.Frame(packet, now + 500 * ms, false) && policy.Waiting(),
              "checksum-invalid and AFE SOS data cannot complete recovery");
        Check(policy.Next(stream, now + ms + Policy::kReadyBudgetNs, true) == Step::None,
              "ready wait deadline inclusive and blocks additional reset requests");
        Check(policy.Next(stream, now + ms + Policy::kReadyBudgetNs + 1, true) == Step::Fallback,
              "missing new usable packet has bounded fallback even after stopped/ready events");
        Check(!policy.Frame(packet, now + ms + Policy::kReadyBudgetNs + 2, true),
              "late packet cannot undo an expired reconstruction budget");
        Policy successful;
        Check(successful.Next(stream, now, true) == Step::Request && successful.Result(0, now + ms) &&
              successful.Frame(packet, now + 600 * ms, true) && !successful.Waiting(),
              "only newer epoch usable data within the deadline completes reconstruction");
    }
    for (bool stable : {false, true}) {
        Policy policy;
        uint64_t now = base;
        uint64_t epoch = 1;
        auto stream = Running(epoch, now);
        for (unsigned i = 0; i < Policy::kMaxAccepted; ++i) {
            now += 5000 * ms;
            Check(policy.Next(stream, now, true) == Step::Request && policy.Result(0, now + ms),
                  "bounded accepted recovery attempt");
            packet.epoch = ++epoch;
            packet.timestamp_ns = now += 500 * ms;
            stream = Running(epoch, now);
            Check(stream.Accept(packet, now) && policy.Frame(packet, now, true),
                  "new epoch valid data releases wait without forgiving repeated stalls");
        }
        if (stable) {
            for (unsigned second = 0; second < 30; ++second) {
                now += 1000 * ms;
                Check(!policy.Frame(packet, now, true), "stable packets are not additional recoveries");
            }
        } else {
            // Large elapsed wall time is insufficient if invalid data breaks
            // the continuous good window.
            policy.Frame(packet, now + Policy::kStableNs - 1, false);
            now += Policy::kStableNs;
            policy.Frame(packet, now, true);
        }
        Check(policy.Next(stream, now + 5000 * ms, true) == (stable ? Step::Request : Step::Fallback),
              "three accepted restarts stay bounded until thirty seconds of uninterrupted good frames");
    }
    {
        Policy policy;
        auto stream = Running(1, base);
        Check(policy.Next(stream, base + 5000 * ms, true) == Step::Request, "time fixture");
        Check(!policy.Result(0, base) && policy.Next(stream, base + 5001 * ms, true) == Step::Fallback,
              "backwards completion cannot be admitted");
    }
    std::cout << "In-process AFE request/race budgets, old-epoch fence, validity proof and supervisor fallback passed\n";
}
