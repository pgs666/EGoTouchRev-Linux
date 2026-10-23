// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include "hx-v4l2-uapi.h"

namespace himax {

class Capture {
public:
    ~Capture();
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
    Capture() = default;
    bool Open(const std::string& device, bool buffers = true);
    bool Start();
    bool RefreshInfo();
    bool Frame(hx_raw_frame& frame);
    bool Event(hx_raw_state& state);
    bool Command(uint64_t epoch, uint64_t sequence, hx_raw_action action, uint32_t param);
    bool Rearm(uint64_t epoch, uint64_t sequence);
    // Success only schedules AFE reconstruction; preserve errno on failure.
    bool Restart(uint64_t epoch, uint64_t sequence);
    int Fd() const { return m_fd; }
    const hx_raw_info& Info() const { return m_info; }
    const std::string& Error() const { return m_error; }
    const std::string& Path() const { return m_path; }

private:
    bool Fail(const char* operation);
    struct Mapping { void* data = nullptr; size_t size = 0; };
    std::array<Mapping, HX_RAW_QUEUE_DEPTH> m_buffers{};
    hx_raw_info m_info{};
    int m_fd = -1;
    bool m_streaming = false;
    std::string m_error;
    std::string m_path;
};

}  // namespace himax
