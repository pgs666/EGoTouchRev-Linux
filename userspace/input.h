// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <linux/input.h>
#include <span>
#include <string>

namespace himax {

struct Contact {
    uint8_t id = 0;
    uint16_t x = 0;  // Original panel direction, 0..25599 and 0..15999.
    uint16_t y = 0;
    uint16_t pressure = 0;
};

// Pure slot bookkeeping, tested without creating a real input device.
class SlotState {
public:
    SlotState() { m_ids.fill(-1); }
    bool Frame(std::span<const Contact> contacts);
    std::span<const input_event> Events() const { return {m_events.data(), m_count}; }
    bool Active() const;

private:
    void Event(uint16_t type, uint16_t code, int32_t value);
    std::array<int8_t, 10> m_ids{};
    std::array<input_event, 96> m_events{};
    size_t m_count = 0;
};

class Input {
public:
    ~Input();
    bool Open();
    bool Report(std::span<const Contact> contacts);
    bool Release() { return Report({}); }
    bool Active() const { return m_slots.Active(); }
    const std::string& Error() const { return m_error; }

private:
    bool Fail(const char* operation);
    int m_fd = -1;
    SlotState m_slots;
    std::string m_error;
};

}  // namespace himax
