// SPDX-License-Identifier: MIT
#include "recording.h"
#include "hx-v4l2-uapi.h"
#include "raw-payload.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <sstream>
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

static int Replay(const char* program, const char* path, bool grace,
                  std::string* diagnostic = nullptr, const char* project = nullptr) {
    int logs[2];
    Check(pipe(logs) == 0, "replay diagnostic pipe");
    const pid_t child = fork();
    Check(child >= 0, "fork replay");
    if (!child) {
        const int quiet = open("/dev/null", O_WRONLY);
        if (quiet < 0 || dup2(quiet, STDOUT_FILENO) < 0 || dup2(logs[1], STDERR_FILENO) < 0)
            _exit(127);
        close(quiet);
        close(logs[0]);
        close(logs[1]);
        if (grace) execl(program, program, "--allow-startup-invalid", "--hxst", path, nullptr);
        else if (project) execl(program, program, "--hxst", path, project, nullptr);
        else execl(program, program, "--hxst", path, nullptr);
        _exit(127);
    }
    close(logs[1]);
    std::string text;
    char buffer[512];
    ssize_t bytes;
    while ((bytes = read(logs[0], buffer, sizeof(buffer))) != 0) {
        if (bytes < 0) {
            if (errno == EINTR) continue;
            Check(false, "read replay diagnostics");
        }
        text.append(buffer, bytes);
        Check(text.size() < 16384, "bounded replay diagnostics");
    }
    close(logs[0]);
    if (diagnostic) *diagnostic = std::move(text);
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
    packet.flags = HX_RAW_FRAME_CHARGER | HX_RAW_FRAME_SOS | HX_RAW_FRAME_REARM;
    packet.payload[40] = 123;
    file.Write(&packet, sizeof(packet));
    himax::RecordedFrame frame;
    {
        himax::RecordingReader reader(file.path, himax::RecordingFormat::Hxst, 0);
        Check(reader.Next(frame) && frame.timeNs == packet.timestamp_ns &&
              frame.epoch == 7 && frame.sos && frame.charger && frame.rearm &&
              frame.length == HX_RAW_PAYLOAD_SIZE && frame.payload[40] == 123,
              "HXST preserves payload and lifecycle metadata");
        Check(!reader.Next(frame) && reader.Error().empty(), "clean EOF");
    }
    for (bool oldVersion : {false, true}) {
        auto invalid = packet;
        if (oldVersion) invalid.version = 1;
        else invalid.flags |= 1U << 31;
        file.Write(&invalid, sizeof(invalid));
        himax::RecordingReader reader(file.path, himax::RecordingFormat::Hxst, 0);
        Check(!reader.Next(frame) && !reader.Error().empty(),
              "unknown flags and pre-v2 origin claims are rejected");
    }
    for (uint32_t size : {HX_RAW_MASTER_SIZE, HX_RAW_PAYLOAD_SIZE}) {
        auto sized = packet;
        sized.payload_size = size;
        file.Write(&sized, sizeof(sized));
        himax::RecordingReader reader(file.path, himax::RecordingFormat::Hxst, 0);
        Check(himax::ValidPayload(sized) && reader.Next(frame) && frame.length == size &&
              frame.payload[40] == 123, "new and legacy captures preserve actual payload size");
    }
    for (uint32_t size : {0u, HX_RAW_MASTER_SIZE - 1, HX_RAW_MASTER_SIZE + 1,
                          HX_RAW_PAYLOAD_SIZE + 1, UINT32_MAX}) {
        auto sized = packet;
        sized.payload_size = size;
        file.Write(&sized, sizeof(sized));
        himax::RecordingReader reader(file.path, himax::RecordingFormat::Hxst, 0);
        Check(!himax::ValidPayload(sized) && !reader.Next(frame) && !reader.Error().empty(),
              "unknown and overflowing payload lengths rejected before padding access");
    }
    for (size_t offset : {size_t(HX_RAW_MASTER_SIZE), size_t(HX_RAW_PAYLOAD_SIZE - 1)}) {
        auto padded = packet;
        padded.payload_size = HX_RAW_MASTER_SIZE;
        padded.payload[offset] = 1;
        file.Write(&padded, sizeof(padded));
        himax::RecordingReader reader(file.path, himax::RecordingFormat::Hxst, 0);
        Check(!himax::ValidPayload(padded) && !reader.Next(frame) && !reader.Error().empty(),
              "uncaptured tail cannot masquerade as zero padding");
        padded.payload_size = HX_RAW_PAYLOAD_SIZE;
        Check(himax::ValidPayload(padded), "legacy captured tail remains opaque evidence");
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
    for (uint32_t size : {HX_RAW_MASTER_SIZE, HX_RAW_PAYLOAD_SIZE}) {
        auto sized = packet;
        sized.payload_size = size;
        file.Write(&sized, sizeof(sized));
        Check(Replay(argv[1], file.path, false) == 0,
              "strict solver replay supports both captured master lengths");
    }
    std::array<hx_raw_frame, 6> packets;
    packets.fill(packet);
    for (auto& bad : packets) bad.payload[2] ^= 1;
    file.Write(packets.data(), 4 * sizeof(packet));
    Check(Replay(argv[1], file.path, true) == 1, "all-invalid capture fails even with grace");
    packets[4] = packet;
    file.Write(packets.data(), 5 * sizeof(packet));
    Check(Replay(argv[1], file.path, false) == 1, "strict replay rejects startup corruption");
    Check(Replay(argv[1], file.path, true) == 0, "four startup transients followed by good frame allowed");
    file.Write(packets.data(), sizeof(packets));
    Check(Replay(argv[1], file.path, true) == 1, "post-start corruption is never waived");
    packets[4].payload[2] ^= 1;
    packets[5] = packet;
    file.Write(packets.data(), sizeof(packets));
    Check(Replay(argv[1], file.path, true) == 1, "five startup errors exceed recovery threshold");
    packets.fill(packet);
    std::fill(std::begin(packets[0].payload), std::end(packets[0].payload), 0xec);
    file.Write(packets.data(), 2 * sizeof(packet));
    std::string fillerDiagnostic;
    Check(Replay(argv[1], file.path, true, &fillerDiagnostic) == 1 &&
          fillerDiagnostic.find("bus-filler") != std::string::npos,
          "even one bus-filler frame is never waived as startup calibration");
    file.Write(&packet, sizeof(packet) - 1);
    Check(Replay(argv[1], file.path, true) == 1, "grace never tolerates truncated metadata");
    // Captures carry the AFE-selected identity so BOE data cannot silently
    // run with the default CSOT gains during replay.
    std::ostringstream encoded(std::ios::binary);
    Check(himax::WriteHxstHeader(encoded, "W273AS1300", HX_RAW_INVERT_X | HX_RAW_SWAP_XY),
          "HXRC panel header written");
    encoded.write(reinterpret_cast<const char*>(&packet), sizeof(packet));
    const auto container = encoded.str();
    Check(container.size() == 32 + sizeof(packet), "fixed container header size");
    file.Write(container.data(), container.size());
    {
        himax::RecordingReader reader(file.path, himax::RecordingFormat::Hxst, 0);
        Check(reader.Error().empty() && reader.ProjectId() == "W273AS1300" &&
              reader.CoordinateFlags() == (HX_RAW_INVERT_X | HX_RAW_SWAP_XY),
              "panel identity and firmware coordinate mapping survive recording");
        Check(reader.Next(frame) && frame.timeNs == packet.timestamp_ns &&
              frame.payload[0] == 0x5a && !reader.Next(frame) && reader.Error().empty(),
              "header leaves frame payload and count unchanged");
    }
    std::string diagnostic;
    Check(Replay(argv[1], file.path, false, &diagnostic) == 0 &&
          diagnostic.find("profile=W273AS1300") != std::string::npos,
          "replay automatically initializes BOE profile from header");
    Check(Replay(argv[1], file.path, false, &diagnostic, "W273AS2700") == 0 &&
          diagnostic.find("Overriding recorded project") != std::string::npos &&
          diagnostic.find("profile=W273AS2700") != std::string::npos,
          "explicit profile override is visible and honored");
    auto corrupted = container;
    corrupted[4] = 2;
    file.Write(corrupted.data(), corrupted.size());
    {
        himax::RecordingReader reader(file.path, himax::RecordingFormat::Hxst, 0);
        Check(!reader.Error().empty(), "unknown container version rejected");
    }
    corrupted = container;
    corrupted[28] = 8;
    file.Write(corrupted.data(), corrupted.size());
    {
        himax::RecordingReader reader(file.path, himax::RecordingFormat::Hxst, 0);
        Check(!reader.Error().empty(), "unknown recorded coordinate flags rejected");
    }
    file.Write(container.data(), 12);
    {
        himax::RecordingReader reader(file.path, himax::RecordingFormat::Hxst, 0);
        Check(!reader.Error().empty(), "truncated container header rejected");
    }
    packet.version = 1;
    file.Write(&packet, sizeof(packet));
    {
        himax::RecordingReader reader(file.path, himax::RecordingFormat::Hxst, 0);
        Check(reader.ProjectId().empty() && reader.Next(frame) && reader.Error().empty(),
              "legacy headerless ABI-v1 recordings remain readable");
    }
    std::ostringstream refused;
    Check(!himax::WriteHxstHeader(refused, "bad/id") && refused.str().empty(),
          "invalid project is rejected before writing");
    Check(!himax::WriteHxstHeader(refused, "W273AS2700", 8) && refused.str().empty(),
          "unknown firmware flags rejected before writing");
    refused.setstate(std::ios::badbit);
    Check(!himax::WriteHxstHeader(refused, "W273AS2700"), "header I/O failure propagated");
    std::cout << "Recording validation passed\n";
}
