// SPDX-License-Identifier: MIT
#include "startup-drain-host.h"
#include "FrameLayout.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>

static void Check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

struct Fixture {
    struct Operation {
        unsigned channel, operation, value;
        bool operator==(const Operation&) const = default;
    };
    std::vector<Operation> operations;
    unsigned failedReads = 0, reads = 0, opens = 0, failAt = 0;
    unsigned waitMs = 0;
    bool failReopen = false;
    static int Call(void* context, unsigned channel, unsigned operation, unsigned value) {
        auto& self = *static_cast<Fixture*>(context);
        self.operations.push_back({channel, operation, value});
        if (self.failAt == self.operations.size()) return -1;
        if (operation == HX_DRAIN_OPEN) {
            ++self.opens;
            if (self.failReopen && self.opens > 2) return -1;
        }
        if (operation == HX_DRAIN_FRAME) {
            const bool failed = self.failedReads & (1u << self.reads++);
            if (failed) self.waitMs += 200; // Scripted GetFrame timeout, not a real wait.
            return failed ? -1 : 0;
        }
        return 0;
    }
    int Run(unsigned& discarded) {
        hx_startup_drain_bus bus{this, Call};
        return hx_windows_startup_drain(&bus, &discarded);
    }
};

int main() {
    using Op = Fixture::Operation;
    const std::vector<Op> setup = {
        {0, HX_DRAIN_OPEN, 0}, {1, HX_DRAIN_OPEN, 0},
        {0, HX_DRAIN_VALID, 0}, {0, HX_DRAIN_BLOCK, 1}, {0, HX_DRAIN_TIMEOUT, 200},
        {1, HX_DRAIN_VALID, 0}, {1, HX_DRAIN_BLOCK, 1}, {1, HX_DRAIN_TIMEOUT, 200},
    };
    for (unsigned failed = 0; failed < 64; ++failed) for (bool failReopen : {false, true}) {
        Fixture f;
        f.failedReads = failed;
        f.failReopen = failReopen;
        unsigned discarded = ~0u, failures = 0;
        Check(f.Run(discarded) == 0, "cleanup remains best-effort on frame/reopen failure");
        auto expected = setup;
        for (unsigned i = 0; i < 6; ++i) {
            const unsigned channel = (i & 1) ? 0 : 1;
            const unsigned bytes = channel ? Frame::kSlaveFrameSize : Frame::kMasterFrameSize;
            expected.push_back({channel, HX_DRAIN_FRAME, bytes});
            if (failed & (1u << i)) {
                ++failures;
                expected.push_back({channel, HX_DRAIN_OPEN, 0});
            }
        }
        Check(f.operations == expected, "real Windows opens/configures, then consumes slave/master three times");
        Check(f.reads == 6 && discarded == 6 - failures,
              "three attempts per channel, not three checksum-valid frames");
        Check(f.waitMs == failures * 200 && f.opens == 2 + failures,
              "timeout requests no-reset producer kick without adding a fixed 8ms sleep");
    }
    for (unsigned fail = 1; fail <= setup.size(); ++fail) {
        Fixture f;
        f.failAt = fail;
        unsigned discarded = ~0u;
        Check(f.Run(discarded) != 0 && f.reads == 0 && discarded == ~0u,
              "open/read-policy failure aborts before any frame cleanup");
        std::vector<Op> expected(setup.begin(), setup.begin() + fail);
        if (fail != 1) {
            expected.push_back({1, HX_DRAIN_CLOSE, 0});
            expected.push_back({0, HX_DRAIN_CLOSE, 0});
        }
        Check(f.operations == expected, "read policy or slave-open failure closes both channels");
    }
    std::cout << "Original Windows startup queue drain: ordered policy, all 64 read outcomes, "
                 "failed reopens, early failure/close handling and 200ms waits passed\n";
}
