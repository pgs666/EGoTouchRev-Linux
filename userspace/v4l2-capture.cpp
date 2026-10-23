// SPDX-License-Identifier: MIT
#include "v4l2-capture.h"
#include "raw-payload.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <glob.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace himax {

Capture::~Capture() {
    if (m_fd >= 0 && m_streaming) {
        int type = V4L2_BUF_TYPE_META_CAPTURE;
        ioctl(m_fd, VIDIOC_STREAMOFF, &type);
    }
    for (const auto& buffer : m_buffers)
        if (buffer.data) munmap(buffer.data, buffer.size);
    if (m_fd >= 0) close(m_fd);
}

bool Capture::Fail(const char* operation) {
    m_error = std::string(operation) + ": " + std::strerror(errno);
    return false;
}

bool Capture::Open(const std::string& device, bool buffers) {
    if (m_fd >= 0) {
        m_error = "Capture already open";
        return false;
    }
    if (device.empty()) {
        glob_t paths{};
        glob("/dev/video*", 0, nullptr, &paths);
        std::string found;
        unsigned int count = 0;
        for (size_t i = 0; i < paths.gl_pathc; ++i) {
            const int fd = open(paths.gl_pathv[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            v4l2_capability caps{};
            if (ioctl(fd, VIDIOC_QUERYCAP, &caps) == 0 &&
                std::strncmp(reinterpret_cast<char*>(caps.driver), "himax-spi", sizeof(caps.driver)) == 0 &&
                (caps.device_caps & V4L2_CAP_META_CAPTURE)) {
                ++count;
                found = paths.gl_pathv[i];
            }
            close(fd);
        }
        globfree(&paths);
        if (count != 1) {
            m_error = "Specify --device: expected exactly one Himax V4L2 metadata device";
            return false;
        }
        return Open(found, buffers);
    }
    m_fd = open(device.c_str(), (buffers ? O_RDWR : O_RDONLY) | O_NONBLOCK | O_CLOEXEC);
    if (m_fd < 0) return Fail("open");
    m_path = device;
    v4l2_capability caps{};
    if (ioctl(m_fd, VIDIOC_QUERYCAP, &caps)) return Fail("QUERYCAP");
    if (std::strncmp(reinterpret_cast<char*>(caps.driver), "himax-spi", sizeof(caps.driver)) != 0 ||
        !(caps.device_caps & V4L2_CAP_META_CAPTURE) || !(caps.device_caps & V4L2_CAP_STREAMING)) {
        m_error = "Not a Himax raw metadata device";
        return false;
    }
    if (!RefreshInfo()) return false;
    if (!buffers) return true;
    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_META_CAPTURE;
    format.fmt.meta.dataformat = V4L2_META_FMT_HIMAX_SPI;
    if (ioctl(m_fd, VIDIOC_S_FMT, &format)) return Fail("S_FMT");
    if (format.fmt.meta.dataformat != V4L2_META_FMT_HIMAX_SPI ||
        format.fmt.meta.buffersize != sizeof(hx_raw_frame)) {
        m_error = "Incompatible HXST metadata format";
        return false;
    }
    v4l2_event_subscription subscription{};
    subscription.type = V4L2_EVENT_HIMAX_STATE;
    if (ioctl(m_fd, VIDIOC_SUBSCRIBE_EVENT, &subscription)) return Fail("SUBSCRIBE_EVENT");
    v4l2_requestbuffers request{};
    request.type = V4L2_BUF_TYPE_META_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    request.count = m_buffers.size();
    if (ioctl(m_fd, VIDIOC_REQBUFS, &request)) return Fail("REQBUFS");
    if (request.count < 2 || request.count > m_buffers.size()) {
        m_error = "Unexpected V4L2 buffer count";
        return false;
    }
    for (uint32_t i = 0; i < request.count; ++i) {
        v4l2_buffer buffer{};
        buffer.type = request.type;
        buffer.memory = request.memory;
        buffer.index = i;
        if (ioctl(m_fd, VIDIOC_QUERYBUF, &buffer)) return Fail("QUERYBUF");
        if (buffer.length < sizeof(hx_raw_frame)) {
            m_error = "Short V4L2 mapping";
            return false;
        }
        void* p = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, buffer.m.offset);
        if (p == MAP_FAILED) return Fail("mmap");
        m_buffers[i] = {p, buffer.length};
        if (ioctl(m_fd, VIDIOC_QBUF, &buffer)) return Fail("QBUF");
    }
    return true;
}

bool Capture::Start() {
    int type = V4L2_BUF_TYPE_META_CAPTURE;
    if (ioctl(m_fd, VIDIOC_STREAMON, &type)) return Fail("STREAMON");
    m_streaming = true;
    return RefreshInfo();
}

bool Capture::RefreshInfo() {
    if (ioctl(m_fd, VIDIOC_HIMAX_INFO, &m_info)) return Fail("HIMAX_INFO");
    if (m_info.size != sizeof(m_info) || m_info.version != HX_RAW_ABI_VERSION ||
        m_info.rows != 40 || m_info.cols != 60 || !ValidPayloadSize(m_info.payload_size) ||
        (m_info.coordinate_flags & ~HX_RAW_COORDINATE_FLAGS) || m_info.project_id[15]) {
        m_error = "Incompatible Himax info";
        return false;
    }
    return true;
}

bool Capture::Frame(hx_raw_frame& frame) {
    v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_META_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    if (ioctl(m_fd, VIDIOC_DQBUF, &buffer)) {
        if (errno == EAGAIN || errno == EINTR) return false;
        return Fail("DQBUF");
    }
    if (buffer.index >= m_buffers.size() || !m_buffers[buffer.index].data ||
        buffer.bytesused != sizeof(frame) || buffer.flags & V4L2_BUF_FLAG_ERROR) {
        m_error = "Invalid V4L2 buffer";
        return false;
    }
    std::memcpy(&frame, m_buffers[buffer.index].data, sizeof(frame));
    if (ioctl(m_fd, VIDIOC_QBUF, &buffer)) return Fail("QBUF");
    if (frame.size != sizeof(frame) || frame.version != HX_RAW_ABI_VERSION ||
        frame.payload_size != m_info.payload_size || !ValidPayload(frame) || frame.reserved[0] ||
        frame.reserved[1] || frame.reserved_tail ||
        frame.flags & ~HX_RAW_FRAME_FLAGS) {
        m_error = "Invalid HXST metadata";
        return false;
    }
    return true;
}

bool Capture::Event(hx_raw_state& state) {
    v4l2_event event{};
    if (ioctl(m_fd, VIDIOC_DQEVENT, &event)) {
        if (errno == ENOENT || errno == EAGAIN || errno == EINTR) return false;
        return Fail("DQEVENT");
    }
    if (event.type != V4L2_EVENT_HIMAX_STATE) return false;
    std::memcpy(&state, event.u.data, sizeof(state));
    if (state.version != HX_RAW_ABI_VERSION || state.running > 1) {
        m_error = "Invalid Himax lifecycle event";
        return false;
    }
    return true;
}

bool Capture::Command(uint64_t epoch, uint64_t sequence, hx_raw_action action, uint32_t param) {
    hx_raw_command command{};
    command.size = sizeof(command);
    command.version = HX_RAW_ABI_VERSION;
    command.epoch = epoch;
    command.sequence = sequence;
    command.action = action;
    command.parameter = param;
    return ioctl(m_fd, VIDIOC_HIMAX_COMMAND, &command) == 0;
}

bool Capture::Rearm(uint64_t epoch, uint64_t sequence) {
    hx_raw_rearm request{};
    request.size = sizeof(request);
    request.version = HX_RAW_ABI_VERSION;
    request.epoch = epoch;
    request.sequence = sequence;
    return ioctl(m_fd, VIDIOC_HIMAX_REARM_TAGGED, &request) == 0;
}

}  // namespace himax
