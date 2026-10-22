// SPDX-License-Identifier: MIT
#include "spi-host.h"

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

struct Fake {
    struct Transaction {
        std::vector<uint8_t> wire;
        bool read;
        bool operator==(const Transaction&) const = default;
    };
    std::vector<Transaction> transactions;
    unsigned int failures = 0;
    int failure = -EIO;
    bool corruptOnError = false;
    int shortBy = 0;
    hx_spi_test_bus bus{this, Transfer};

    static int Transfer(void* context, const void* tx, void* rx, u32 length, u32* actual) {
        auto& self = *static_cast<Fake*>(context);
        auto input = static_cast<const uint8_t*>(tx);
        self.transactions.push_back({{input, input + length}, rx != nullptr});
        if (self.transactions.size() <= self.failures) {
            if (rx && self.corruptOnError) std::fill_n(static_cast<uint8_t*>(rx), length, 0xbb);
            *actual = length / 2;
            return self.failure;
        }
        *actual = length - self.shortBy;
        if (rx) {
            const auto copied = std::min(length, *actual);
            for (u32 i = 0; i < copied; ++i)
                static_cast<uint8_t*>(rx)[i] = static_cast<uint8_t>((i * 17 + 5) % 251);
        }
        return 0;
    }
};

struct Native {
    Fake fake;
    std::vector<uint8_t> storage;
    spi_device device{&fake.bus};
    himax_ts_data ts{};

    explicit Native(u32 capacity = 16384) : storage(capacity + 32, 0xa7) {
        ts.spi = &device;
        ts.xfer_buf = storage.data() + 16;
        ts.xfer_buf_sz = capacity;
    }
    void CheckGuards() const {
        Check(std::all_of(storage.begin(), storage.begin() + 16,
                          [](auto v) { return v == 0xa7; }), "scratch leading guard");
        Check(std::all_of(storage.end() - 16, storage.end(),
                          [](auto v) { return v == 0xa7; }), "scratch trailing guard");
    }
};

static void ReadComparison(uint8_t command, u32 length, unsigned int failures = 0,
                           int shortBy = 0, int failure = -EIO) {
    Native native;
    Fake reference;
    native.fake.failures = reference.failures = failures;
    native.fake.shortBy = reference.shortBy = shortBy;
    native.fake.failure = reference.failure = failure;
    std::vector<uint8_t> actual(length + 2, 0xa7), expected = actual;
    const int result = himax_spi_read(&native.ts, command, actual.data() + 1, length);
    const int oracle = hx_spi_windows_read(&reference.bus, command, expected.data() + 1, length);
    Check((result == 0) == (oracle == 0), "read success matches Windows");
    Check(actual == expected, "read data and untouched failure evidence match Windows");
    Check(native.fake.transactions == reference.transactions, "read wire bytes and retries match Windows");
    native.CheckGuards();
}

static void WriteComparison(uint8_t command, u32 length, bool address, int failure = 0) {
    Native native;
    Fake reference;
    if (failure) {
        native.fake.failures = reference.failures = 10;
        native.fake.failure = reference.failure = failure;
    }
    const std::array<uint8_t, 4> addr{0x94, 0x72, 0, 0x10};
    std::vector<uint8_t> payload(length);
    for (u32 i = 0; i < length; ++i) payload[i] = static_cast<uint8_t>(i * 23 + 7);
    // A zero-length data pointer is immaterial to either implementation.
    auto* data = length ? payload.data() : nullptr;
    const auto* addrPtr = address ? addr.data() : nullptr;
    const int result = hx_spi_native_write(&native.ts, command, addrPtr, data, length);
    const int oracle = hx_spi_windows_write(&reference.bus, command, addrPtr, data, length);
    Check((result == 0) == (oracle == 0), "write success matches Windows");
    Check(native.fake.transactions == reference.transactions, "write header/address/data and no-retry match Windows");
    native.CheckGuards();
}

int main() {
    for (unsigned int command = 0; command <= 255; ++command) {
        for (u32 length : {0u, 1u, 2u, 4u, 255u, 256u, 5132u, 16378u}) {
            ReadComparison(command, length);
            WriteComparison(command, length, false);
            WriteComparison(command, length, true);
        }
    }
    // Every successful retry position, and exhaustion. Error/cancel byte
    // identity is compared separately: Windows loses native errno detail.
    for (unsigned int failures = 0; failures <= 11; ++failures)
        ReadComparison(0x30, 5132, failures);
    for (int failure : {-EIO, -ETIMEDOUT, -EAGAIN}) {
        ReadComparison(0x08, 4, 9, 0, failure);
        ReadComparison(0x08, 4, 10, 0, failure);
        WriteComparison(0x00, 4, true, failure);
    }
    ReadComparison(0x30, 5132, 10, 0, -ECANCELED);
    // Length is checked AFTER the successful OS transaction, not retried.
    for (int shortBy = 1; shortBy <= 7; ++shortBy) {
        ReadComparison(0x08, 4, 0, shortBy);
        ReadComparison(0x08, 4, 2, shortBy);
    }
    for (int shortBy : {1, 256, 5132, 5135})
        ReadComparison(0x30, 5132, 2, shortBy);

    for (int terminal : {-ECANCELED, -EINTR, -ERESTARTSYS, -ENODEV, -ESHUTDOWN}) {
        Native native;
        native.fake.failure = terminal;
        native.fake.failures = 20;
        std::array<uint8_t, 8> output{};
        Check(himax_spi_read(&native.ts, 0x30, output.data(), output.size()) == terminal,
              "preserve terminal native errno");
        Check(native.fake.transactions.size() == 1, "terminal native I/O is not retried");
    }
    for (u32 capacity : {0u, 1u, 2u, 3u, 7u, 16384u}) {
        for (u32 length : {0u, 1u, 4u, 5132u, 16381u, 16382u, UINT32_MAX}) {
            Native native(capacity);
            std::vector<uint8_t> output(16384, 0xa7);
            const bool valid = capacity >= 3 && length <= capacity - 3;
            const int result = himax_spi_read(&native.ts, 0x30, output.data(), length);
            Check(valid ? result == 0 : result == -EMSGSIZE, "read capacity checked before addition");
            Check(valid || native.fake.transactions.empty(), "invalid read has no hardware I/O");
            native.CheckGuards();
        }
        for (bool address : {false, true}) {
            Native native(capacity);
            std::array<uint8_t, 4> addr{};
            uint8_t data = 5;
            const u32 overhead = address ? 6 : 2;
            const auto* addrPtr = address ? addr.data() : nullptr;
            Check(hx_spi_native_write(&native.ts, 0, addrPtr, &data, UINT32_MAX) == -EMSGSIZE,
                  "maximal write length rejected before addition");
            Check(native.fake.transactions.empty(), "invalid write has no hardware I/O");
            const int result = hx_spi_native_write(&native.ts, 0, addrPtr, &data, 1);
            Check(capacity >= overhead + 1 ? result == 0 : result == -EMSGSIZE,
                  "write accounts for optional address and header");
            native.CheckGuards();
        }
    }
    // RX/TX alias: the controller may damage the command on failure. The
    // native retry rebuilds it, deliberately stronger than the Windows wrapper.
    Native aliased;
    aliased.fake.corruptOnError = true;
    aliased.fake.failures = 9;
    std::array<uint8_t, 32> output{};
    Check(himax_spi_read(&aliased.ts, 0x30, output.data(), output.size()) == 0,
          "damaged read header recovers on final retry");
    Check(std::all_of(aliased.fake.transactions.begin(), aliased.fake.transactions.end(),
        [&](const auto& transaction) { return transaction == aliased.fake.transactions.front(); }),
        "RX overwrite never corrupts a retried header or dummy payload");
    // Destination may be the same driver scratch area on successful reads.
    Native overlap;
    Check(himax_spi_read(&overlap.ts, 0x08, overlap.ts.xfer_buf, 4) == 0,
          "scratch destination supports memmove overlap");
    for (unsigned int i = 0; i < 4; ++i)
        Check(overlap.ts.xfer_buf[i] == ((i + 3) * 17 + 5) % 251, "overlapping output bytes");
    for (int shortBy : {1, 2, 6}) {
        Native native;
        Fake reference;
        native.fake.shortBy = reference.shortBy = shortBy;
        const std::array<uint8_t, 4> data{};
        Check(hx_spi_native_write(&native.ts, 0x31, nullptr, data.data(), 4) == -EIO,
              "native short writes fail without retry");
        Check(hx_spi_windows_write(&reference.bus, 0x31, nullptr, data.data(), 4) == 0,
              "explicit oracle boundary: Windows does not check write length");
        Check(native.fake.transactions == reference.transactions, "short write attempted exactly once");
    }
    Native overreported;
    overreported.fake.shortBy = -1;
    Check(himax_spi_read(&overreported.ts, 0x08, output.data(), 4) == -EIO,
          "impossible overreported native length is rejected too");
    std::cout << "SPI: 256 commands, wire lengths/address/payload, all ten retries, short reads, "
                 "cancellation, capacity/overflow and alias checks passed\n";
    std::cout << "Native extensions verified: stopped/removed controller, rebuilt retry headers, "
                 "short write rejection (not claimed Windows-equivalent)\n";
}
