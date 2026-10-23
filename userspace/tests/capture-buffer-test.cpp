// SPDX-License-Identifier: MIT
// Actual Capture code with only the OS/device boundary replaced by link wraps.
#include "v4l2-capture.h"

#include <array>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <sys/mman.h>

namespace {
void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
struct Device {
    unsigned returned = 2, requests = 0, queries = 0, queues = 0;
    unsigned unmapped = 0, closed = 0, stopped = 0;
    unsigned failedMap = 99;
    unsigned restarts = 0;
    int restartError = 0;
    hx_raw_restart restart{};
    std::array<hx_raw_frame*, 2> mappings{};
    std::deque<unsigned> completed;
} device;

hx_raw_frame Packet(uint64_t sequence) {
    hx_raw_frame packet{};
    packet.size = sizeof(packet);
    packet.version = HX_RAW_ABI_VERSION;
    packet.epoch = 7;
    packet.sequence = sequence;
    packet.timestamp_ns = 1000000000 + sequence;
    packet.payload_size = HX_RAW_MASTER_SIZE;
    packet.payload[0] = sequence; // Capture validates metadata, solver validates checksum.
    return packet;
}
} // namespace

extern "C" int __wrap_open(const char* path, int, ...) {
    Check(!std::strcmp(path, "/test-himax"), "no unrelated real file access");
    return 91;
}
extern "C" int __wrap_close(int fd) {
    Check(fd == 91, "close exact opened descriptor");
    ++device.closed;
    return 0;
}
extern "C" void* __wrap_mmap(void*, size_t size, int, int, int fd, off_t offset) {
    Check(fd == 91 && size == sizeof(hx_raw_frame) && offset >= 0 && offset < 2,
          "exact MMAP frame layout and index");
    if (unsigned(offset) == device.failedMap) { errno = ENOMEM; return MAP_FAILED; }
    auto* frame = new hx_raw_frame{};
    device.mappings[offset] = frame;
    return frame;
}
extern "C" int __wrap_munmap(void* pointer, size_t size) {
    Check(size == sizeof(hx_raw_frame), "unmap exact frame size");
    ++device.unmapped;
    delete static_cast<hx_raw_frame*>(pointer);
    return 0;
}
extern "C" int __wrap_ioctl(int fd, unsigned long operation, ...) {
    Check(fd == 91, "only test capture descriptor");
    va_list arguments;
    va_start(arguments, operation);
    void* data = va_arg(arguments, void*);
    va_end(arguments);
    switch (operation) {
    case VIDIOC_QUERYCAP: {
        auto& caps = *static_cast<v4l2_capability*>(data);
        std::memcpy(caps.driver, "himax-spi", 10);
        caps.device_caps = V4L2_CAP_META_CAPTURE | V4L2_CAP_STREAMING;
        return 0;
    }
    case VIDIOC_HIMAX_INFO: {
        auto& info = *static_cast<hx_raw_info*>(data);
        info = {};
        info.size = sizeof(info);
        info.version = HX_RAW_ABI_VERSION;
        info.rows = 40;
        info.cols = 60;
        info.payload_size = HX_RAW_MASTER_SIZE;
        std::memcpy(info.project_id, "W273AS2700", 11);
        return 0;
    }
    case VIDIOC_S_FMT:
        static_cast<v4l2_format*>(data)->fmt.meta.buffersize = sizeof(hx_raw_frame);
        return 0;
    case VIDIOC_SUBSCRIBE_EVENT: return 0;
    case VIDIOC_REQBUFS: {
        auto& request = *static_cast<v4l2_requestbuffers*>(data);
        Check(request.count == 2 && request.memory == V4L2_MEMORY_MMAP,
              "real Capture requests exactly two buffers");
        ++device.requests;
        request.count = device.returned;
        return 0;
    }
    case VIDIOC_QUERYBUF: {
        auto& buffer = *static_cast<v4l2_buffer*>(data);
        Check(buffer.index < 2, "no out-of-bounds buffer query");
        ++device.queries;
        buffer.length = sizeof(hx_raw_frame);
        buffer.m.offset = buffer.index;
        return 0;
    }
    case VIDIOC_QBUF: {
        const auto& buffer = *static_cast<v4l2_buffer*>(data);
        Check(buffer.index < 2 && device.mappings[buffer.index], "only mapped buffer is requeued");
        ++device.queues;
        // Model immediate reuse: the caller must copy BEFORE requeuing.
        *device.mappings[buffer.index] = {};
        return 0;
    }
    case VIDIOC_DQBUF: {
        if (device.completed.empty()) { errno = EAGAIN; return -1; }
        auto& buffer = *static_cast<v4l2_buffer*>(data);
        buffer.index = device.completed.front();
        device.completed.pop_front();
        buffer.bytesused = sizeof(hx_raw_frame);
        return 0;
    }
    case VIDIOC_STREAMON: return 0;
    case VIDIOC_STREAMOFF: ++device.stopped; return 0;
    case VIDIOC_HIMAX_RESTART:
        ++device.restarts;
        device.restart = *static_cast<hx_raw_restart*>(data);
        if (device.restartError) { errno = device.restartError; return -1; }
        return 0;
    default: Check(false, "unexpected capture ioctl"); return -1;
    }
}

int main() {
    for (unsigned count : {0u, 1u, 3u, 4u}) {
        device = {};
        device.returned = count;
        {
            himax::Capture capture;
            Check(!capture.Open("/test-himax") && capture.Error() == "Unexpected V4L2 buffer count",
                  "bad allocation counts are rejected before mapping");
        }
        Check(device.requests == 1 && !device.queries && !device.unmapped && device.closed == 1,
              "failed count negotiation leaks neither mapping nor descriptor");
    }
    for (unsigned failed : {0u, 1u}) {
        device = {};
        device.failedMap = failed;
        {
            himax::Capture capture;
            Check(!capture.Open("/test-himax") && capture.Error().starts_with("mmap:"),
                  "partial MMAP failure is reported");
        }
        Check(device.unmapped == failed && device.closed == 1,
              "partial allocation rollback unmaps only successfully mapped buffers");
    }
    device = {};
    {
        himax::Capture capture;
        Check(capture.Open("/test-himax") && capture.Start(), "two-buffer stream starts");
        Check(device.requests == 1 && device.queries == 2 && device.queues == 2,
              "both buffers mapped and queued exactly once at startup");
        for (uint64_t sequence : {uint64_t(0), uint64_t(123)}) {
            Check(capture.Restart(7, sequence) && device.restart.size == sizeof(hx_raw_restart) &&
                  device.restart.version == HX_RAW_ABI_VERSION && device.restart.epoch == 7 &&
                  device.restart.sequence == sequence && !device.restart.reserved[0] &&
                  !device.restart.reserved[1], "restart wrapper sends exact token and zero reserved fields");
        }
        for (int error : {ENOTTY, ESTALE, EAGAIN, EBUSY, ENODEV, EIO}) {
            device.restartError = error;
            Check(!capture.Restart(7, 0) && errno == error && capture.Error().empty(),
                  "restart failure preserves errno including unsupported old module, not fatal capture error");
        }
        Check(device.restarts == 8 && device.stopped == 0 && device.queues == 2,
              "wrapper never silently retries, restarts STREAMON or mutates queue");
        *device.mappings[0] = Packet(1);
        *device.mappings[1] = Packet(2);
        device.completed = {0, 1};
        hx_raw_frame frame{};
        Check(capture.Frame(frame) && frame.sequence == 1 && frame.payload[0] == 1,
              "oldest frame is privately copied before buffer reuse");
        Check(capture.Frame(frame) && frame.sequence == 2 && frame.payload[0] == 2,
              "second retained frame remains ordered");
        Check(!capture.Frame(frame) && capture.Error().empty(), "empty queue is nonfatal EAGAIN");
        *device.mappings[0] = Packet(23);
        device.completed = {0};
        Check(capture.Frame(frame) && frame.sequence == 23 && frame.payload[0] == 23,
              "producer gaps are preserved, not relabelled contiguous");
    }
    Check(device.unmapped == 2 && device.closed == 1 && device.stopped == 1,
          "destructor stops stream, unmaps both buffers and closes descriptor");
    std::cout << "Actual Capture two-buffer negotiation, FIFO copies, gaps and failure cleanup passed\n";
}
