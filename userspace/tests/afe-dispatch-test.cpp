// SPDX-License-Identifier: MIT
#include "afe-dispatch-host.h"
#include "afe-action.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <vector>

static void Check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

struct Bus {
    struct Operation {
        u32 address;
        bool read;
        std::vector<u8> bytes;
        bool operator==(const Operation&) const = default;
    };
    std::vector<Operation> operations;
    unsigned failAt = 0;
    static int Write(void* context, u32 address, const u8* data, size_t length) {
        auto& self = *static_cast<Bus*>(context);
        self.operations.push_back({address, false, {data, data + length}});
        return self.operations.size() == self.failAt ? -EREMOTEIO : 0;
    }
    static int Read(void* context, u32 address, u8* data, size_t length) {
        auto& self = *static_cast<Bus*>(context);
        self.operations.push_back({address, true, std::vector<u8>(length, 0xec)});
        if (self.operations.size() == self.failAt) return -EREMOTEIO;
        std::fill_n(data, length, 0xec); // Readback is transport, not an echo/completion ACK.
        return 0;
    }
    hx_dispatch_device Device(u8 slot = 0) { return {{this, Write, Read}, slot, 1, 0}; }
};

int main() {
    using Action = Solvers::Tsa::AfeActionType;
    struct Vector { Action solver; hx_raw_action host; u8 command; };
    constexpr std::array vectors = {
        Vector{Action::ClearStatus, HX_RAW_CLEAR_STATUS, 0x06},
        Vector{Action::ForceToFreqPoint, HX_RAW_FORCE_FREQ, 0x0c},
        Vector{Action::ForceToScanRate, HX_RAW_FORCE_RATE, 0x0e},
        Vector{Action::EnterIdle, HX_RAW_ENTER_IDLE, 0x0a},
        Vector{Action::StartCalibration, HX_RAW_CALIBRATE, 0x01},
    };
    for (const auto& v : vectors) {
        Check(himax::HardwareAction(v.solver) == v.host, "actual solver-to-host mapping");
        for (unsigned value = 0; value < 256; ++value) {
            const bool allowed = v.host == HX_RAW_CLEAR_STATUS ? value == 4 || value == 8 :
                                 v.host == HX_RAW_FORCE_FREQ ? value < 2 : value == 0;
            for (u8 slot = 0; slot < 5; ++slot) for (unsigned fail = 0; fail < 4; ++fail) {
                Bus native, windows;
                native.failAt = windows.failAt = fail;
                auto n = native.Device(slot), w = windows.Device(slot);
                const int nr = hx_dispatch_native(&n, himax::HardwareAction(v.solver), value);
                const int wr = hx_dispatch_windows(&w, v.host, value);
                if (!allowed) {
                    Check(nr == -EINVAL && native.operations.empty() && n.slot == slot,
                          "unsupported native parameters cause no I/O or slot mutation");
                    if (v.host == HX_RAW_FORCE_FREQ)
                        Check(wr != 0 && windows.operations.empty(), "Windows also rejects unsupported frequency");
                    continue;
                }
                Check((nr == 0) == (wr == 0) && n.slot == w.slot &&
                      native.operations == windows.operations,
                      "supported actions match Windows register bytes, ordering and slot on every fault");
                Check(native.operations.size() == (fail ? fail : 3), "stop at first transport failure");
                Check(native.operations[0].bytes[2] == v.command &&
                      native.operations[0].bytes[4] == value, "expected command/value reach mailbox body");
                Check(nr == (fail ? -EREMOTEIO : 0), "native transport errno preserved");
                Check(n.slot == (fail ? slot : (slot + 1) % 5), "slot commit only after full transport");
                Check(w.idle_retry == (v.host == HX_RAW_ENTER_IDLE && !fail),
                      "Windows arms IDLE retry only after a successful send");
                Bus disconnected;
                auto d = disconnected.Device(slot);
                d.connected = 0;
                Check(hx_dispatch_windows(&d, v.host, value) != 0 && disconnected.operations.empty(),
                      "Windows connection gate is separate from native owner/epoch hardware gate");
            }
        }
    }
    for (u32 action : {u32(HX_RAW_RESET), 255u, UINT32_MAX}) {
        Bus bus;
        auto device = bus.Device();
        Check(hx_dispatch_native(&device, action, 0) == -EINVAL && bus.operations.empty(),
              "reset/unknown action never sent to firmware mailbox");
    }
    for (const auto& v : vectors) for (u32 value : {256u, 65536u, UINT32_MAX}) {
        Bus bus;
        auto device = bus.Device();
        Check(hx_dispatch_native(&device, v.host, value) == -EINVAL && bus.operations.empty(),
              "wide UAPI parameter cannot silently truncate to byte");
    }
    Check(himax::HardwareAction(static_cast<Action>(255)) == HX_RAW_RESET,
          "unknown solver action sentinel remains rejected by host");
    std::cout << "Actual solver/native/Windows AFE dispatch: five actions, byte-domain parameters, "
                 "five slots, transport faults, disconnected Windows and invalid UAPI passed\n";
}
