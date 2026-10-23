// SPDX-License-Identifier: MIT
#include "input.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

namespace himax {

void SlotState::Event(uint16_t type, uint16_t code, int32_t value) {
    auto& event = m_events[m_count++];
    event = {};
    event.input_event_sec = m_timestampNs / 1000000000;
    event.input_event_usec = m_timestampNs % 1000000000 / 1000;
    event.type = type;
    event.code = code;
    event.value = value;
}

bool SlotState::Frame(std::span<const Contact> contacts, uint64_t timestampNs) {
    if ((m_flags & ~HX_RAW_COORDINATE_FLAGS) || contacts.size() > m_ids.size()) return false;
    uint32_t ids = 0;
    for (const auto& c : contacts) {
        if (c.id >= 32 || c.x > 25599 || c.y > 15999 || c.pressure > 4095 ||
            (ids & (1u << c.id))) return false;
        ids |= 1u << c.id;
    }
    // An input-only release may be newer than the next queued raw sample.
    m_timestampNs = std::max(m_timestampNs, timestampNs);
    m_count = 0;
    // mousedev expects BTN_TOUCH first in each synchronization frame.
    Event(EV_KEY, BTN_TOUCH, contacts.empty() ? 0 : 1);
    Event(EV_KEY, BTN_TOOL_FINGER, contacts.empty() ? 0 : 1);
    for (size_t slot = 0; slot < m_ids.size(); ++slot) {
        if (m_ids[slot] < 0 || ids & (1u << m_ids[slot])) continue;
        Event(EV_ABS, ABS_MT_SLOT, slot);
        Event(EV_ABS, ABS_MT_TRACKING_ID, -1);
        m_ids[slot] = -1;
    }
    for (const auto& raw : contacts) {
        const auto c = Transform(raw);
        auto found = std::find(m_ids.begin(), m_ids.end(), c.id);
        const bool fresh = found == m_ids.end();
        if (fresh) found = std::find(m_ids.begin(), m_ids.end(), -1);
        const auto slot = static_cast<size_t>(found - m_ids.begin());
        Event(EV_ABS, ABS_MT_SLOT, slot);
        if (fresh) {
            m_ids[slot] = c.id;
            Event(EV_ABS, ABS_MT_TRACKING_ID, c.id);
        }
        Event(EV_ABS, ABS_MT_POSITION_X, c.x);
        Event(EV_ABS, ABS_MT_POSITION_Y, c.y);
        Event(EV_ABS, ABS_MT_PRESSURE, c.pressure);
    }
    if (!contacts.empty()) {
        const auto first = Transform(contacts[0]);
        Event(EV_ABS, ABS_X, first.x);
        Event(EV_ABS, ABS_Y, first.y);
    }
    Event(EV_SYN, SYN_REPORT, 0);
    return true;
}

Contact SlotState::Transform(Contact contact) const {
    // Match touchscreen_apply_prop_to_x_y(): invert in the original sensor
    // axes first, then swap. Desktop/compositor rotation is a separate step.
    if (m_flags & HX_RAW_INVERT_X) contact.x = 25599 - contact.x;
    if (m_flags & HX_RAW_INVERT_Y) contact.y = 15999 - contact.y;
    if (m_flags & HX_RAW_SWAP_XY) std::swap(contact.x, contact.y);
    return contact;
}

bool SlotState::Active() const {
    return std::any_of(m_ids.begin(), m_ids.end(), [](int id) { return id >= 0; });
}

bool Input::Fail(const char* operation) {
    m_error = std::string(operation) + ": " + std::strerror(errno);
    return false;
}

Input::~Input() {
    if (m_fd >= 0) {
        Release();
        ioctl(m_fd, UI_DEV_DESTROY);
        close(m_fd);
    }
}

bool Input::Open(uint32_t flags) {
    if (m_fd >= 0 || (flags & ~HX_RAW_COORDINATE_FLAGS)) {
        m_error = "Invalid input initialization";
        return false;
    }
    m_slots = SlotState(flags);
    m_fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
    if (m_fd < 0) return Fail("uinput open");
    if (ioctl(m_fd, UI_SET_EVBIT, EV_ABS) || ioctl(m_fd, UI_SET_EVBIT, EV_KEY) ||
        ioctl(m_fd, UI_SET_KEYBIT, BTN_TOUCH) || ioctl(m_fd, UI_SET_KEYBIT, BTN_TOOL_FINGER) ||
        ioctl(m_fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT)) return Fail("uinput capabilities");
    struct Axis { uint16_t code; int max; int resolution; };
    const Axis axes[] = {
        {ABS_MT_SLOT, 9, 0}, {ABS_MT_TRACKING_ID, 65535, 0},
        {ABS_MT_POSITION_X, m_slots.XMax(), 96}, {ABS_MT_POSITION_Y, m_slots.YMax(), 96},
        {ABS_MT_PRESSURE, 4095, 0}, {ABS_X, m_slots.XMax(), 96}, {ABS_Y, m_slots.YMax(), 96},
    };
    for (const auto& axis : axes) {
        if (ioctl(m_fd, UI_SET_ABSBIT, axis.code)) return Fail("uinput ABS bit");
        uinput_abs_setup setup{};
        setup.code = axis.code;
        setup.absinfo.maximum = axis.max;
        setup.absinfo.resolution = axis.resolution;
        if (ioctl(m_fd, UI_ABS_SETUP, &setup)) return Fail("uinput ABS setup");
    }
    uinput_setup setup{};
    std::strcpy(setup.name, "Himax Capacitive TouchScreen");
    setup.id.bustype = BUS_SPI;
    setup.id.vendor = 0x4858;
    setup.id.product = 0x8312;
    setup.id.version = 0x0140;
    if (ioctl(m_fd, UI_DEV_SETUP, &setup) || ioctl(m_fd, UI_DEV_CREATE))
        return Fail("uinput create");
    return true;
}

bool Input::Report(std::span<const Contact> contacts, uint64_t timestampNs) {
    if (!timestampNs) {
        timespec now{};
        if (clock_gettime(CLOCK_MONOTONIC, &now)) return Fail("input timestamp");
        timestampNs = static_cast<uint64_t>(now.tv_sec) * 1000000000 + now.tv_nsec;
    }
    if (!m_slots.Frame(contacts, timestampNs)) {
        m_error = "Invalid contact snapshot";
        return false;
    }
    if (m_fd < 0) {
        m_error = "Input device is not open";
        return false;
    }
    const auto events = m_slots.Events();
    const auto* data = reinterpret_cast<const uint8_t*>(events.data());
    size_t remaining = events.size_bytes();
    while (remaining) {
        const ssize_t count = write(m_fd, data, remaining);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return Fail("uinput write");
        remaining -= count;
        data += count;
    }
    return true;
}

}  // namespace himax
