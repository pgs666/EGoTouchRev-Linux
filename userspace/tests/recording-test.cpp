// SPDX-License-Identifier: MIT
#include "recording.h"
#include "hx-v4l2-uapi.h"

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <sys/wait.h>
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

static int Replay(const char* program, const char* path, bool grace) {
    const pid_t child = fork();
    Check(child >= 0, "fork replay");
    if (!child) {
        const int quiet = open("/dev/null", O_WRONLY);
        if (quiet < 0 || dup2(quiet, STDOUT_FILENO) < 0 || dup2(quiet, STDERR_FILENO) < 0)
            _exit(127);
        close(quiet);
        if (grace) execl(program, program, "--allow-startup-invalid", "--hxst", path, nullptr);
        else execl(program, program, "--hxst", path, nullptr);
        _exit(127);
    }
    int status;
    pid_t result;
    do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
    Check(result == child && WIFEXITED(status), "replay exited normally");
    return WEXITSTATUS(status);
}

int main(int argc, char** argv) {
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
    Check(argc == 2, "replay executable supplied");
    packet.reserved[0] = 0;
    packet.flags = 0;
    packet.payload[0] = 0x5a;
    packet.payload[1] = 0xa5;
    uint32_t sum = 0;
    for (size_t i = 4; i < 4804; i += 2) {
        packet.payload[i] = 0xfe;
        packet.payload[i + 1] = 0x7f;
    }
    for (size_t i = 4; i < himax::kNativeMasterSize; i += 2)
        sum += packet.payload[i] | packet.payload[i + 1] << 8;
    const uint16_t checksum = static_cast<uint16_t>(0u - sum);
    packet.payload[2] = checksum & 0xff;
    packet.payload[3] = checksum >> 8;
    std::array<hx_raw_frame, 6> packets;
    packets.fill(packet);
    for (auto& bad : packets) bad.payload[0] = 0;
    file.Write(packets.data(), 4 * sizeof(packet));
    Check(Replay(argv[1], file.path, true) == 1, "all-invalid capture fails even with grace");
    packets[4] = packet;
    file.Write(packets.data(), 5 * sizeof(packet));
    Check(Replay(argv[1], file.path, false) == 1, "strict replay rejects startup corruption");
    Check(Replay(argv[1], file.path, true) == 0, "four startup transients followed by good frame allowed");
    file.Write(packets.data(), sizeof(packets));
    Check(Replay(argv[1], file.path, true) == 1, "post-start corruption is never waived");
    packets[4].payload[0] = 0;
    packets[5] = packet;
    file.Write(packets.data(), sizeof(packets));
    Check(Replay(argv[1], file.path, true) == 1, "five startup errors exceed recovery threshold");
    file.Write(&packet, sizeof(packet) - 1);
    Check(Replay(argv[1], file.path, true) == 1, "grace never tolerates truncated metadata");
    std::cout << "Recording validation passed\n";
}
