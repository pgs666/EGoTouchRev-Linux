// SPDX-License-Identifier: MIT
#include "input.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace himax {

void SlotState::Event(uint16_t type, uint16_t code, int32_t value) {
    m_events[m_count++] = input_event{{0, 0}, type, code, value};
}

bool SlotState::Frame(std::span<const Contact> contacts) {
    if (contacts.size() > m_ids.size()) return false;
    uint32_t ids = 0;
    for (const auto& c : contacts) {
        if (c.id >= 32 || c.x > 25599 || c.y > 15999 || c.pressure > 4095 ||
            (ids & (1u << c.id))) return false;
        ids |= 1u << c.id;
    }
    m_count = 0;
    for (size_t slot = 0; slot < m_ids.size(); ++slot) {
        if (m_ids[slot] < 0 || ids & (1u << m_ids[slot])) continue;
        Event(EV_ABS, ABS_MT_SLOT, slot);
        Event(EV_ABS, ABS_MT_TRACKING_ID, -1);
        m_ids[slot] = -1;
    }
    for (const auto& c : contacts) {
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
    Event(EV_KEY, BTN_TOUCH, contacts.empty() ? 0 : 1);
    Event(EV_KEY, BTN_TOOL_FINGER, contacts.empty() ? 0 : 1);
    if (!contacts.empty()) {
        Event(EV_ABS, ABS_X, contacts[0].x);
        Event(EV_ABS, ABS_Y, contacts[0].y);
    }
    Event(EV_SYN, SYN_REPORT, 0);
    return true;
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

bool Input::Open() {
    m_fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
    if (m_fd < 0) return Fail("uinput open");
    if (ioctl(m_fd, UI_SET_EVBIT, EV_ABS) || ioctl(m_fd, UI_SET_EVBIT, EV_KEY) ||
        ioctl(m_fd, UI_SET_KEYBIT, BTN_TOUCH) || ioctl(m_fd, UI_SET_KEYBIT, BTN_TOOL_FINGER) ||
        ioctl(m_fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT)) return Fail("uinput capabilities");
    struct Axis { uint16_t code; int max; int resolution; };
    const Axis axes[] = {
        {ABS_MT_SLOT, 9, 0}, {ABS_MT_TRACKING_ID, 65535, 0},
        {ABS_MT_POSITION_X, 25599, 96}, {ABS_MT_POSITION_Y, 15999, 96},
        {ABS_MT_PRESSURE, 4095, 0}, {ABS_X, 25599, 96}, {ABS_Y, 15999, 96},
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

bool Input::Report(std::span<const Contact> contacts) {
    if (!m_slots.Frame(contacts)) {
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
