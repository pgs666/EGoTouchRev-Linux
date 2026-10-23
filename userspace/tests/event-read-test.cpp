// SPDX-License-Identifier: MIT
#include "spi-host.h"
#include "hx-v4l2-uapi.h"
#include "native-frame.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

struct EventBus {
    static constexpr u32 kWireSize = Frame::kMasterFrameSize;
    static constexpr u32 kPrefixSize = 3;
    static constexpr u32 kMasterSize = kWireSize - kPrefixSize;
    std::array<u8, kMasterSize> master{};
    std::vector<std::vector<u8>> transactions;
    unsigned int failures = 0;
    int error = -EIO;
    u32 shortBy = 0;
    hx_spi_test_bus bus{this, Transfer};

    EventBus() {
        master[0] = 0x5a;
        master[1] = 0xa5;
        for (u32 i = 4; i < kMasterSize; ++i)
            master[i] = static_cast<u8>(i * 17 + 5);
        u32 sum = 0;
        for (u32 i = 4; i < kMasterSize; i += 2)
            sum += static_cast<uint16_t>(master[i] | (master[i + 1] << 8));
        const auto checksum = static_cast<uint16_t>(0u - sum);
        master[2] = checksum & 0xff;
        master[3] = checksum >> 8;
    }

    static int Transfer(void* context, const void* tx, void* rx, u32 length, u32* actual) {
        auto& self = *static_cast<EventBus*>(context);
        const auto* bytes = static_cast<const u8*>(tx);
        self.transactions.emplace_back(bytes, bytes + length);
        Check(length >= kPrefixSize && rx, "event transaction is a prefixed read");
        Check(bytes[0] == 0xf3 && bytes[1] == 0x30 && bytes[2] == 0,
              "master event command is F3 30 00");
        Check(std::all_of(bytes + kPrefixSize, bytes + length,
                          [](u8 v) { return v == 0; }), "dummy TX bytes are initialized");
        // Damage the aliased scratch buffer on failure, as a real controller can.
        std::fill_n(static_cast<u8*>(rx), length, 0xec);
        if (self.transactions.size() <= self.failures) {
            *actual = length / 2;
            return self.error;
        }
        Check(self.shortBy <= length, "short-read injection fits transaction");
        *actual = length - self.shortBy;
        for (u32 i = kPrefixSize; i < *actual; ++i)
            static_cast<u8*>(rx)[i] = self.master[(i - kPrefixSize) % kMasterSize];
        return 0;
    }
};

struct NativeEvent {
    EventBus fake;
    spi_device spi{&fake.bus};
    std::vector<u8> scratch;
    std::array<u8, HX_RAW_PAYLOAD_SIZE + 32> evidence;
    himax_ts_data ts{};

    explicit NativeEvent(u32 capacity) : scratch(capacity + 32, 0xa7) {
        evidence.fill(0xb6);
        ts.spi = &spi;
        ts.xfer_buf = scratch.data() + 16;
        ts.xfer_buf_sz = capacity;
        ts.event_buf = evidence.data() + 16;
        ts.event_buf_sz = HX_RAW_PAYLOAD_SIZE;
    }
    void CheckGuards() const {
        Check(std::all_of(scratch.begin(), scratch.begin() + 16,
                          [](u8 v) { return v == 0xa7; }) &&
              std::all_of(scratch.end() - 16, scratch.end(),
                          [](u8 v) { return v == 0xa7; }), "SPI scratch bounds");
        Check(std::all_of(evidence.begin(), evidence.begin() + 16,
                          [](u8 v) { return v == 0xb6; }) &&
              std::all_of(evidence.end() - 16, evidence.end(),
                          [](u8 v) { return v == 0xb6; }), "event evidence bounds");
    }
};

int main() {
    static_assert(EventBus::kMasterSize == himax::kNativeMasterSize);
    static_assert(EventBus::kMasterSize == HX_RAW_MASTER_SIZE);
    static_assert(HX_RAW_PAYLOAD_SIZE == himax::kNativeTransportSize);
    // Begin at the old probe capacity to reproduce its 5129 + 3 byte reads.
    for (u32 capacity : {5132u, EventBus::kWireSize, 5135u, 16384u}) {
        NativeEvent native(capacity);
        Check(hx_spi_native_read_event(&native.ts) == 0, "whole event read succeeds");
        Check(native.fake.transactions.size() == 1, "one event uses exactly one SPI transaction");
        Check(native.fake.transactions.front().size() == EventBus::kWireSize,
              "wire size equals pinned Windows master frame size");
        Check(std::equal(native.fake.master.begin(), native.fake.master.end(), native.ts.event_buf),
              "checksum, raw matrix and full suffix preserved");
        Check(std::all_of(native.ts.event_buf + EventBus::kMasterSize,
                          native.ts.event_buf + HX_RAW_PAYLOAD_SIZE,
                          [](u8 v) { return v == 0; }), "ABI tail is zero padding, not another event");
        std::array<u8, Frame::kMasterFrameSize> thp{};
        Check(himax::ToThpMaster({native.ts.event_buf, HX_RAW_PAYLOAD_SIZE}, thp) ==
                  himax::FrameError::None, "existing parser accepts padded ABI record");
        Check(std::equal(native.fake.master.begin(), native.fake.master.end(), thp.begin() + 3),
              "THP conversion preserves every valid master byte");
        native.CheckGuards();
    }
    for (u32 capacity : {0u, 2u, 3u, 22u, EventBus::kWireSize - 1}) {
        NativeEvent native(capacity);
        const auto evidence = native.evidence;
        Check(hx_spi_native_read_event(&native.ts) == -EMSGSIZE,
              "insufficient controller capacity rejects rather than splitting a frame");
        Check(native.fake.transactions.empty(), "capacity failure performs no SPI I/O");
        Check(native.evidence == evidence, "capacity failure preserves last complete event");
        native.CheckGuards();
    }
    for (u32 shortBy : {1u, 3u, EventBus::kWireSize}) {
        NativeEvent native(EventBus::kWireSize);
        const auto evidence = native.evidence;
        native.fake.shortBy = shortBy;
        Check(hx_spi_native_read_event(&native.ts) == -EIO, "short event read fails");
        Check(native.fake.transactions.size() == 1, "short success is not retried");
        Check(native.evidence == evidence, "short read preserves whole previous event including tail");
        native.CheckGuards();
    }
    for (unsigned int failures : {1u, 9u, 10u}) {
        NativeEvent native(EventBus::kWireSize);
        native.fake.failures = failures;
        const auto evidence = native.evidence;
        const int result = hx_spi_native_read_event(&native.ts);
        Check(result == (failures < 10 ? 0 : -EIO), "event read retains ten-attempt policy");
        Check(native.fake.transactions.size() == std::min(failures + 1, 10u),
              "retry count bounded");
        Check(std::all_of(native.fake.transactions.begin(), native.fake.transactions.end(),
                          [&](const auto& tx) { return tx == native.fake.transactions.front(); }),
              "every retry starts a whole master transaction with a fresh command");
        if (failures == 10) Check(native.evidence == evidence, "exhaustion preserves prior frame");
        native.CheckGuards();
    }
    NativeEvent cancelled(EventBus::kWireSize);
    const auto evidence = cancelled.evidence;
    cancelled.fake.failures = 10;
    cancelled.fake.error = -ESHUTDOWN;
    Check(hx_spi_native_read_event(&cancelled.ts) == -ESHUTDOWN &&
          cancelled.fake.transactions.size() == 1 && cancelled.evidence == evidence,
          "controller shutdown stops immediately and preserves evidence");
    std::cout << "Production event read: one complete Windows-sized master transaction, "
                 "ABI padding, checksum/suffix, bounds, retries and failure evidence passed\n";
}
