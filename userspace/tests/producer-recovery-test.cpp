// SPDX-License-Identifier: MIT
#include "producer-recovery.h"
#include "frame-read-mode.h"
#include <cstdlib>
#include <iostream>
#include <array>

extern "C" unsigned hx_windows_rearm_schedule(bool idle, unsigned elapsed,
                                              unsigned* schedule, unsigned capacity);
extern "C" bool hx_windows_idle_retry(const uint8_t* thp, bool previous);

static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

static void ReadMode() {
    using Error = himax::FrameError;
    std::array<uint8_t, Frame::kMasterFrameSize> thp{};
    constexpr auto offset = Frame::kMasterSuffixOffset + 2 * Frame::MasterWord::kIdleFlag;
    const auto suffix = [&](uint16_t value) {
        thp[offset] = value & 0xff;
        thp[offset + 1] = value >> 8;
    };
    for (bool prior : {false, true}) {
        for (unsigned word = 0; word <= UINT16_MAX; ++word) {
            himax::FrameReadMode mode;
            mode.Action(HX_RAW_ENTER_IDLE, prior);
            suffix(word);
            mode.Observe(Error::None, thp);
            Check(mode.IdleRetry() == hx_windows_idle_retry(thp.data(), prior),
                  "all suffix words and both previous modes match compiled Windows update");
        }
        for (const auto error : {Error::Size, Error::Sync, Error::Checksum,
                                  Error::AllZero, Error::BusFiller}) {
            for (bool rearm : {false, true}) {
                himax::FrameReadMode mode;
                mode.Action(HX_RAW_ENTER_IDLE, prior);
                suffix(prior ? 0 : 1); // A failed conversion leaves stale THP data.
                mode.Observe(error, thp, rearm);
                Check(mode.IdleRetry() == (himax::IsEmptyRearm(error, rearm) && prior),
                      "invalid master clears retry except explicit tagged no-data; never reads stale THP");
            }
        }
    }
    himax::FrameReadMode mode;
    mode.Action(HX_RAW_ENTER_IDLE, false);
    Check(!mode.IdleRetry(), "failed EnterIdle cannot arm retry");
    mode.Action(HX_RAW_ENTER_IDLE, true);
    Check(mode.IdleRetry(), "successful EnterIdle arms before firmware confirmation");
    mode.Action(HX_RAW_ENTER_IDLE, false);
    Check(mode.IdleRetry(), "failed retry preserves an already armed mode");
    for (const auto action : {HX_RAW_CLEAR_STATUS, HX_RAW_FORCE_FREQ, HX_RAW_FORCE_RATE,
                               HX_RAW_CALIBRATE, HX_RAW_RESET}) {
        mode.Action(action, true);
        Check(mode.IdleRetry(), "unrelated transport completion is not an AFE lifecycle reset");
    }
    suffix(0x100); // Do not truncate to a byte and accidentally infer active.
    mode.Observe(Error::None, thp);
    Check(mode.IdleRetry(), "unknown suffix retains command-side retry");
    suffix(0);
    mode.Observe(Error::None, thp);
    Check(!mode.IdleRetry(), "valid active suffix cancels pending idle retry");
    suffix(1);
    mode.Observe(Error::None, thp, true);
    Check(mode.IdleRetry(), "valid tagged data updates the read mode normally");
    mode.Reset();
    Check(!mode.IdleRetry(), "stopped/new epoch clears previous retry hint");

    // Regression: command transport succeeded but no IDLE confirmation IRQ
    // arrives. It must get Windows's long wait, rather than an active 200ms kick.
    constexpr uint64_t base = 1000000000, ms = 1000000;
    using Step = himax::ProducerRecovery::Step;
    himax::StreamState stream;
    stream.State({HX_RAW_ABI_VERSION, 1, 1}, base);
    himax::ProducerRecovery recovery;
    mode.Action(HX_RAW_ENTER_IDLE, true);
    for (unsigned elapsed : {0u, 199u, 200u, 3000u, 3050u, 3599u})
        Check(recovery.Next(stream, base + elapsed * ms, mode.IdleRetry()) == Step::None,
              "command-armed retry tolerates delayed acknowledgement and normal IDLE gaps");
    Check(recovery.Next(stream, base + 3600 * ms, mode.IdleRetry()) == Step::Rearm,
          "missing acknowledgement still has bounded IDLE recovery");
    hx_raw_frame packet{};
    packet.epoch = 1;
    packet.sequence = 1;
    packet.timestamp_ns = base + 3610 * ms;
    Check(stream.Accept(packet, packet.timestamp_ns), "active data resumes after idle wait");
    suffix(0);
    mode.Observe(Error::None, thp);
    recovery.Frame(true);
    Check(recovery.Next(stream, packet.timestamp_ns, mode.IdleRetry()) == Step::None,
          "active receipt starts a new producer wait");
    Check(recovery.Next(stream, packet.timestamp_ns + 199 * ms, mode.IdleRetry()) == Step::None &&
          recovery.Next(stream, packet.timestamp_ns + 200 * ms, mode.IdleRetry()) == Step::Rearm,
          "active suffix restores short wait immediately");
    mode.Action(HX_RAW_ENTER_IDLE, true);
    stream.State({HX_RAW_ABI_VERSION, 0, 2}, base + 4000 * ms);
    mode.Reset();
    Check(recovery.Next(stream, base + 4500 * ms, mode.IdleRetry()) == Step::None,
          "panel-off cancels read retries");
    stream.State({HX_RAW_ABI_VERSION, 1, 3}, base + 5000 * ms);
    Check(recovery.Next(stream, base + 5200 * ms, mode.IdleRetry()) == Step::Rearm,
          "new epoch without first frame never inherits old IDLE wait");
}

int main() {
    ReadMode();
    using Recovery = himax::ProducerRecovery;
    using Step = Recovery::Step;
    constexpr uint64_t base = 1000000000, ms = 1000000;
    static_assert(sizeof(hx_raw_rearm) == 32);
    himax::StreamState stream;
    Recovery recovery;
    Check(recovery.Next(stream, base, false) == Step::None, "stopped has no timeout");
    stream.State({HX_RAW_ABI_VERSION, 1, 1}, base);
    Recovery disabled;
    for (unsigned time : {0u, 200u, 3600u, 4000u})
        Check(disabled.Next(stream, base + time * ms, false, false) == Step::None,
              "disabled policy cannot request active/IDLE/startup kicks");
    Check(disabled.Next(stream, base + 4001 * ms, false, false) == Step::Exhausted,
          "disabled policy retains four-second supervisor fallback");
    Check(disabled.Next(stream, base + 200 * ms, false, true) == Step::Rearm,
          "explicit experimental enable uses separate rearm budget");
    Check(stream.Sequence() == 0, "startup rearm can address sequence zero");
    Check(recovery.Next(stream, base + 199 * ms, false) == Step::None, "wait full active timeout");
    Check(recovery.Next(stream, base + 200 * ms, false) == Step::Rearm, "missing first IRQ can rearm");
    for (unsigned i = 1; i < 7; ++i) {
        const auto last = base + (200 + (i - 1) * 220) * ms;
        Check(recovery.Next(stream, last + 19 * ms, false) == Step::None, "rearm grace");
        Check(recovery.Next(stream, last + 219 * ms, false) == Step::None, "next read wait");
        Check(recovery.Next(stream, last + 220 * ms, false) == Step::Rearm, "bounded next rearm");
    }
    Check(recovery.Next(stream, base + 1539 * ms, false) == Step::None, "last rearm grace");
    Check(recovery.Next(stream, base + 1540 * ms, false) == Step::Exhausted, "seven reopens exhausted");

    hx_raw_frame frame{};
    frame.epoch = 1;
    frame.sequence = 5;
    frame.timestamp_ns = base + 1600 * ms;
    Check(stream.Accept(frame, frame.timestamp_ns), "producer progress");
    recovery.Frame(true);
    Check(recovery.Next(stream, frame.timestamp_ns, true) == Step::None, "progress cancels attempts");
    Check(recovery.Next(stream, frame.timestamp_ns + 3599 * ms, true) == Step::None, "IDLE quiet is normal");
    Check(recovery.Next(stream, frame.timestamp_ns + 3600 * ms, true) == Step::Rearm, "IDLE gets one kick");
    Check(recovery.Next(stream, frame.timestamp_ns + 3619 * ms, true) == Step::None, "IDLE grace");
    Check(recovery.Next(stream, frame.timestamp_ns + 3620 * ms, true) == Step::Exhausted, "IDLE elapsed budget");
    stream.State({HX_RAW_ABI_VERSION, 0, 2}, base + 6000 * ms);
    Check(recovery.Next(stream, base + 9000 * ms, true) == Step::None, "panel off cancels recovery");
    stream.State({HX_RAW_ABI_VERSION, 1, 3}, base + 10000 * ms);
    Check(stream.Sequence() == 0, "new epoch cannot reuse prior sequence");
    Check(recovery.Next(stream, base + 10200 * ms, false) == Step::Rearm, "new epoch new budget");
    recovery.Result(ENOTTY);
    Check(recovery.Next(stream, base + 14000 * ms, false) == Step::None, "old module deadline boundary");
    Check(recovery.Next(stream, base + 14001 * ms, false) == Step::Exhausted, "old module fallback");

    Recovery delayed;
    Check(delayed.Next(stream, base + 15000 * ms, false) == Step::Rearm, "delayed scheduler still tries once");
    Check(delayed.Next(stream, base + 15020 * ms, false) == Step::Exhausted, "delay cannot extend budget");
    Recovery failures;
    Check(failures.Next(stream, base + 10200 * ms, false) == Step::Rearm, "first failure trial");
    failures.Result(EIO);
    stream.State({HX_RAW_ABI_VERSION, 1, 3}, base + 10300 * ms);
    Check(failures.Next(stream, base + 10420 * ms, false) == Step::Rearm, "EIO retries without reset");
    Check(failures.Next(stream, base + 13201 * ms, false) == Step::Rearm, "last kick precedes elapsed-budget check");
    Check(failures.Next(stream, base + 13221 * ms, false) == Step::Exhausted, "duplicate event cannot extend deadline");
    {
        himax::StreamState badStream;
        Recovery badRecovery;
        badStream.State({HX_RAW_ABI_VERSION, 1, 1}, base);
        hx_raw_frame badPacket{};
        badPacket.epoch = 1;
        for (unsigned i = 0; i < 7; ++i) {
            const auto when = base + (200 + i * 201) * ms;
            Check(badRecovery.Next(badStream, when, false) == Step::Rearm, "invalid-data attempt");
            badRecovery.Result(0); // SPI success is not valid data progress.
            badPacket.sequence = i + 1;
            badPacket.timestamp_ns = when + ms;
            Check(badStream.Accept(badPacket, badPacket.timestamp_ns), "fresh transport metadata");
            badRecovery.Frame(false);
        }
        Check(badRecovery.Next(badStream, base + (200 + 7 * 201) * ms, false) == Step::Exhausted,
              "invalid packets cannot perpetually restart rearm budget");
        badPacket.sequence++;
        badPacket.timestamp_ns += 202 * ms;
        Check(badStream.Accept(badPacket, badPacket.timestamp_ns), "valid data after bad series");
        badRecovery.Frame(true);
        Check(badRecovery.Next(badStream, badPacket.timestamp_ns + 200 * ms, false) == Step::Rearm,
              "valid data starts a fresh rearm budget");
    }
    for (bool idle : {false, true}) {
        himax::StreamState polled;
        Recovery policy;
        polled.State({HX_RAW_ABI_VERSION, 1, 1}, base);
        auto when = base + (idle ? 3600 : 200) * ms;
        unsigned attempts = 0;
        for (;;) {
            Check(policy.Next(polled, when, idle) == Step::Rearm, "empty poll attempt starts");
            ++attempts;
            const auto complete = when + 150 * ms; // Real SPI may block.
            policy.Result(0, complete);
            hx_raw_frame p{};
            p.epoch = 1;
            p.sequence = attempts;
            p.flags = HX_RAW_FRAME_REARM;
            p.timestamp_ns = complete + ms;
            Check(polled.Accept(p, p.timestamp_ns), "tagged poll still has valid transport metadata");
            policy.Frame(false);
            policy.EmptyRearm(polled);
            Check(policy.Next(polled, complete + 19 * ms, idle) == Step::None,
                  "empty poll cannot remove post-completion grace");
            if (idle || attempts == 7) {
                Check(policy.Next(polled, complete + 20 * ms, idle) == Step::Exhausted,
                      "empty polling cannot refresh natural producer deadline or attempt budget");
                break;
            }
            Check(policy.Next(polled, complete + 219 * ms, idle) == Step::None,
                  "next timeout starts after ioctl completion, not request start");
            when = complete + 220 * ms;
        }
    }
    for (bool idle : {false, true}) {
        for (unsigned elapsed : {0u, 2600u, 3201u}) {
            std::array<unsigned, 16> expected{};
            const auto count = hx_windows_rearm_schedule(idle, elapsed, expected.data(), expected.size());
            Check(count > 0 && count <= 7, "Windows oracle bounded");
            himax::StreamState oracleStream;
            oracleStream.State({HX_RAW_ABI_VERSION, 1, 1}, base);
            Recovery actual;
            for (unsigned i = 0; i < count; ++i) {
                const auto when = base + expected[i] * ms;
                if (i || !elapsed)
                    Check(actual.Next(oracleStream, when - ms, idle) == Step::None,
                          "not before Windows rearm deadline");
                Check(actual.Next(oracleStream, when, idle) == Step::Rearm, "same rearm times as compiled Windows lambda");
                Check(actual.Next(oracleStream, when + 19 * ms, idle) == Step::None, "same post-rearm grace");
            }
            Check(actual.Next(oracleStream, base + (expected[count - 1] + 20) * ms, idle) == Step::Exhausted,
                  "same final budget/attempt decision as compiled Windows lambda");
        }
    }
    std::cout << "Producer rearm active/IDLE waits, command-side retry, all suffix words, "
                 "budgets, lifecycle and old-module fallback passed\n";
}
