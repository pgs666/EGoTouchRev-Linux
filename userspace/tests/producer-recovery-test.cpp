// SPDX-License-Identifier: MIT
#include "producer-recovery.h"
#include <cstdlib>
#include <iostream>
#include <array>

extern "C" unsigned hx_windows_rearm_schedule(bool idle, unsigned elapsed,
                                              unsigned* schedule, unsigned capacity);

static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

int main() {
    using Recovery = himax::ProducerRecovery;
    using Step = Recovery::Step;
    constexpr uint64_t base = 1000000000, ms = 1000000;
    static_assert(sizeof(hx_raw_rearm) == 32);
    himax::StreamState stream;
    Recovery recovery;
    Check(recovery.Next(stream, base, false) == Step::None, "stopped has no timeout");
    stream.State({HX_RAW_ABI_VERSION, 1, 1}, base);
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
    std::cout << "Producer rearm active/IDLE waits, budgets, lifecycle and old-module fallback passed\n";
}
