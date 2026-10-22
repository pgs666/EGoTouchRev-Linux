// SPDX-License-Identifier: MIT
#include "recording.h"
#include "hx-v4l2-uapi.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <unistd.h>

static void Check(bool value, const char* name) {
    if (!value) { std::cerr << "FAIL: " << name << '\n'; std::exit(1); }
}

class Recording {
public:
    Recording() {
        const int fd = mkstemp(path);
        Check(fd >= 0, "temporary recording");
        close(fd);
    }
    ~Recording() { std::remove(path); }
    void Write(const void* data, size_t size) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char*>(data), size);
        Check(out.good(), "record written");
    }
    char path[40] = "/tmp/himax-recording-test.XXXXXX";
};

int main() {
    Recording file;
    hx_raw_frame packet{};
    packet.size = sizeof(packet);
    packet.version = HX_RAW_ABI_VERSION;
    packet.payload_size = HX_RAW_PAYLOAD_SIZE;
    packet.timestamp_ns = 1234567890;
    packet.epoch = 7;
    packet.sequence = 8;
    packet.flags = HX_RAW_FRAME_CHARGER | HX_RAW_FRAME_SOS;
    packet.payload[40] = 123;
    file.Write(&packet, sizeof(packet));
    himax::RecordedFrame frame;
    {
        himax::RecordingReader reader(file.path, himax::RecordingFormat::Hxst, 0);
        Check(reader.Next(frame) && frame.timeNs == packet.timestamp_ns &&
              frame.epoch == 7 && frame.sos && frame.charger &&
              frame.length == HX_RAW_PAYLOAD_SIZE && frame.payload[40] == 123,
              "HXST preserves payload and lifecycle metadata");
        Check(!reader.Next(frame) && reader.Error().empty(), "clean EOF");
    }
    file.Write(&packet, sizeof(packet) - 1);
    {
        himax::RecordingReader reader(file.path, himax::RecordingFormat::Hxst, 0);
        Check(!reader.Next(frame) && !reader.Error().empty(), "truncated HXST rejected");
    }
    packet.reserved[0] = 1;
    file.Write(&packet, sizeof(packet));
    {
        himax::RecordingReader reader(file.path, himax::RecordingFormat::Hxst, 0);
        Check(!reader.Next(frame) && !reader.Error().empty(), "unknown ABI fields rejected");
    }
    std::array<uint8_t, 40> trace{};
    trace[0] = 'H'; trace[1] = 'X'; trace[2] = 'T'; trace[3] = 'R';
    trace[4] = 1; trace[6] = 40;
    trace[8] = 0xff; trace[9] = 0xff; trace[10] = 0xff; trace[11] = 0xff;
    file.Write(trace.data(), trace.size());
    {
        himax::RecordingReader reader(file.path, himax::RecordingFormat::Trace, 0);
        Check(!reader.Error().empty(), "unbounded HXTR allocation rejected");
    }
    file.Write(packet.payload, sizeof(packet.payload));
    {
        himax::RecordingReader reader(file.path, himax::RecordingFormat::Native, 5132);
        Check(reader.Next(frame) && frame.length == 5132, "native frame readable");
        Check(!reader.Next(frame) && reader.Error().empty(), "native EOF");
    }
    std::cout << "Recording validation passed\n";
}
