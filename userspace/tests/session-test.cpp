// SPDX-License-Identifier: MIT
#include "afe-action.h"
#include "capture-session.h"
#include "solver-screen.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

static void Check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

static int Wait(pid_t child) {
    int status = 0;
    pid_t result;
    do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
    Check(result == child && WIFEXITED(status), "child exited normally");
    return WEXITSTATUS(status);
}

static int Replay(const char* program, const std::string& path, const char* trace = nullptr) {
    const pid_t child = fork();
    Check(child >= 0, "fork replay");
    if (!child) {
        int quiet = open("/dev/null", O_WRONLY);
        if (quiet < 0 || dup2(quiet, STDOUT_FILENO) < 0) _exit(127);
        close(quiet);
        if (trace) execl(program, program, "--session", "--touch-trace", trace, path.c_str(), nullptr);
        else execl(program, program, "--session", path.c_str(), nullptr);
        _exit(127);
    }
    return Wait(child);
}

static void Checksum(hx_raw_frame& packet) {
    uint32_t sum = 0;
    for (size_t i = 4; i < himax::kNativeMasterSize; i += 2)
        sum += packet.payload[i] | packet.payload[i + 1] << 8;
    const uint16_t checksum = static_cast<uint16_t>(0u - sum);
    packet.payload[2] = checksum & 0xff;
    packet.payload[3] = checksum >> 8;
}

static std::string Read(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

static void Write(const std::string& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    Check(output.good(), "write corrupt fixture");
}

int main(int argc, char** argv) {
    Check(argc == 2, "replay supplied");
    char directory[] = "/tmp/himax-session-XXXXXX";
    Check(mkdtemp(directory), "private fixture directory");
    const std::string path = std::string(directory) + "/capture.hxst";
    auto queue = std::make_unique<himax::SessionQueue>();
    himax::SessionEvent event, read;
    for (size_t i = 0; i < himax::SessionQueue::kCapacity; ++i) {
        event.hostTimeNs = i;
        Check(queue->TryPush(event), "fill bounded queue");
    }
    Check(!queue->TryPush(event), "queue overflow never blocks");
    for (size_t i = 0; i < himax::SessionQueue::kCapacity; ++i) {
        Check(queue->TryPop(read) && read.hostTimeNs == i, "queue preserves FIFO evidence");
        event.hostTimeNs = i + 128;
        Check(queue->TryPush(event), "queue wraparound reuses released slot");
    }
    for (size_t i = 0; i < himax::SessionQueue::kCapacity; ++i)
        Check(queue->TryPop(read) && read.hostTimeNs == i + 128, "wrapped FIFO ordering");
    Check(!queue->TryPop(read), "empty queue never blocks");
    std::thread producer([&] {
        himax::SessionEvent item;
        for (uint64_t i = 0; i < 2000; ++i) {
            item.hostTimeNs = i;
            item.packet.sequence = i + 1;
            while (!queue->TryPush(item)) std::this_thread::yield();
        }
    });
    for (uint64_t i = 0; i < 2000; ++i) {
        while (!queue->TryPop(read)) std::this_thread::yield();
        Check(read.hostTimeNs == i && read.packet.sequence == i + 1,
              "concurrent SPSC ordering and publication");
    }
    producer.join();

    {
        himax::SessionRecorder recorder(path);
        Check(!recorder.Error(), "create paired recording exclusively");
        auto solver = std::make_unique<Solvers::Tsa::TsaCoreSolver>();
        auto frame = std::make_unique<Solvers::HeatmapFrame>();
        himax::SolverScreenState screen;
        event = {};
        event.kind = himax::SessionKind::State;
        event.state = {HX_RAW_ABI_VERSION, 0, 1};
        Check(recorder.Submit(event), "panel-off event before identity");
        event.kind = himax::SessionKind::Begin;
        constexpr std::string_view project = "W273AS2700";
        std::copy(project.begin(), project.end(), event.project.begin());
        event.coordinateFlags = 5;
        Check(recorder.Submit(event), "identity queued");
        std::array<uint8_t, Frame::kMasterFrameSize> thp{};
        himax::MasterFrameHealth health;
        unsigned int reports = 0, failedCommands = 0, resets = 0;
        for (unsigned int i = 0; i < 300; ++i) {
            if (i == 0 || i == 125 || i == 161) {
                event = {};
                event.kind = himax::SessionKind::State;
                event.state = {HX_RAW_ABI_VERSION, 0, i == 0 ? 1ULL : i == 125 ? 2ULL : 3ULL};
                Check(recorder.Submit(event), "lifecycle event queued");
                event.state.running = 1;
                Check(recorder.Submit(event), "AFE running before first epoch frame");
                solver->OnAfeStopped();
                if (i) solver->OnAfeSos();
                solver->OnAfeStarted();
                health.Reset();
            }
            event = {};
            auto& packet = event.packet;
            packet.size = sizeof(packet);
            packet.version = HX_RAW_ABI_VERSION;
            packet.payload_size = sizeof(packet.payload);
            packet.epoch = i < 125 ? 1 : i < 161 ? 2 : 3;
            packet.sequence = i + 1;
            packet.timestamp_ns = 1000000000ULL + i * 9000000ULL;
            event.hostTimeNs = packet.timestamp_ns + 1000000;
            packet.flags = (i == 125 || i == 161 ? HX_RAW_FRAME_SOS : 0) |
                           (i >= 200 ? HX_RAW_FRAME_CHARGER : 0);
            packet.payload[0] = 0x5a;
            packet.payload[1] = 0xa5;
            for (size_t j = 4; j < 4804; j += 2) {
                packet.payload[j] = 0xfe;
                packet.payload[j + 1] = 0x7f;
            }
            if (i >= 180 && i < 260) {
                for (int row = 19; row <= 21; ++row) for (int col = 29; col <= 31; ++col) {
                    const uint16_t value = 0x7ffe + (row == 20 && col == 30 ? 1000 : 500);
                    const size_t offset = 4 + 2 * (row * 60 + col);
                    packet.payload[offset] = value & 0xff;
                    packet.payload[offset + 1] = value >> 8;
                }
            }
            packet.payload[4804 + 6] =
                i < 20 || (i >= 125 && i < 145) || (i >= 161 && i < 165) ? 0xbb : 0;
            Checksum(packet);
            if (i >= 120 && i < 125) packet.payload[0] = 0;
            if (i == 160) std::fill(std::begin(packet.payload), std::end(packet.payload), 0xec);
            event.rotation = i / 80;
            screen.SetRotation(event.rotation);
            solver->SetChargerPlugged(packet.flags & HX_RAW_FRAME_CHARGER);
            const auto error = himax::ToThpMaster(packet.payload, thp);
            const bool reset = health.Observe(error);
            if (error != himax::FrameError::None) {
                if (reset) {
                    ++resets;
                    event.actions[event.actionCount++] = {HX_RAW_RESET, 0, 0};
                }
            } else {
                frame->rawPtr = thp.data();
                frame->rawLen = thp.size();
                event.report = screen.Process(*solver, *frame, packet.timestamp_ns / 1000000);
                event.status = solver->LastAfeStatus();
                event.ctrl = solver->LastCtrlFlags();
                event.digest = himax::ContactDigest(*frame);
                reports += event.report;
                for (const auto& action : solver->PendingAfeActions()) {
                    const int error = failedCommands ? 0 : EAGAIN;
                    failedCommands += error != 0;
                    event.actions[event.actionCount++] = {
                        himax::HardwareAction(action.type), action.param, error};
                    solver->OnAfeActionExecuted(action, error == 0);
                }
            }
            Check(recorder.Submit(event), "normal closed-loop frame queued");
            if (i == 100) {
                auto release = event;
                release.kind = himax::SessionKind::Release;
                Check(recorder.Submit(release), "input-only timeout recorded");
            }
        }
        Check(reports > 50 && failedCommands && resets == 2,
              "fixture has contacts, failed command, five-bad recovery and immediate bus SOS");
        Check(recorder.Close(true) && recorder.Dropped() == 0, "complete recording drained");
    }
    Check(Replay(argv[1], path) == 0, "live-output/action/lifecycle replay matches");
    const auto original = Read(path + ".events");
    {
        const auto raw = Read(path);
        const auto trace = std::string(directory) + "/touch-trace.csv";
        Check(Replay(argv[1], path, trace.c_str()) == 0,
              "offline tracing preserves strict live-output verification");
        const auto csv = Read(trace);
        std::istringstream rows(csv);
        std::string row;
        Check(!!std::getline(rows, row) && row.starts_with("frame,epoch,sequence,") &&
              row.ends_with("edge_enabled,max_major_mm"), "touch trace identifies edge policy");
        bool candidate = false, reported = false;
        while (std::getline(rows, row)) {
            std::istringstream fields(row);
            std::vector<std::string> values;
            for (std::string value; std::getline(fields, value, ',');) values.push_back(value);
            Check(values.size() == 44, "fixed trace schema includes unreported candidates");
            candidate |= values[34] == "0";
            reported |= values[34] == "1";
        }
        Check(candidate && reported, "trace shows pre-admission and reported touch states");
        Check(Replay(argv[1], path, trace.c_str()) != 0 && Read(trace) == csv,
              "trace refuses to overwrite existing evidence");
        Check(Replay(argv[1], path, path.c_str()) != 0 && Read(path) == raw &&
              Read(path + ".events") == original, "trace cannot overwrite raw session or sidecar");
    }
    Check(original.starts_with("HXSE 1\n") && original.ends_with("END 300 0 0\n"),
          "explicit complete footer");
    {
        // Removing the first-frame bus-filler reset must not pass strict replay
        // merely because ordinary checksum recovery would wait five frames.
        const auto first = original.find("F 161 ");
        const auto end = original.find('\n', first);
        Check(first != original.npos && end != original.npos, "bus-filler record exists");
        std::istringstream fields(original.substr(first, end - first));
        std::vector<std::string> values;
        for (std::string value; fields >> value;) values.push_back(value);
        Check(values.size() == 12 && values[8] == "1" &&
              values[9] == std::to_string(HX_RAW_RESET), "bus filler requests immediate reset");
        values[8] = "0";
        std::ostringstream withoutReset;
        for (size_t i = 0; i < 9; ++i) withoutReset << values[i] << ' ';
        auto altered = original;
        altered.replace(first, end - first, withoutReset.str());
        Write(path + ".events", altered);
        Check(Replay(argv[1], path) != 0, "missing immediate bus recovery rejected");
        Write(path + ".events", original);
    }
    {
        himax::SessionReader reader(path.c_str());
        size_t frames = 0;
        while (reader.Next(read)) if (read.kind == himax::SessionKind::Frame) {
            Check(read.packet.sequence == ++frames, "raw sequence survives pairing");
        }
        Check(reader.Error().empty() && frames == 300, "paired record count validated");
    }
    {
        himax::SessionRecorder collision(path);
        Check(collision.Error(), "existing evidence is never overwritten");
        Check(Read(path + ".events") == original, "collision preserves evidence");
    }
    for (const auto& corrupt : {
             original.substr(0, original.rfind("END ")),
             original + "R 1\n",
             original.substr(0, original.rfind("END ")) + "END 300 1 0\n",
             original.substr(0, original.rfind("END ")) + "END 299 0 0\n",
             std::string("HXSE 99\n") + original.substr(7)}) {
        Write(path + ".events", corrupt);
        Check(Replay(argv[1], path) != 0, "incomplete or corrupted session never succeeds");
    }
    auto altered = original;
    const auto first = altered.find("F ");
    const auto end = altered.find('\n', first);
    std::istringstream line(altered.substr(first, end - first));
    std::vector<std::string> fields;
    for (std::string field; line >> field;) fields.push_back(field);
    fields[7] = fields[7] == "0" ? "1" : "0";
    std::ostringstream replacement;
    for (const auto& field : fields) replacement << field << ' ';
    altered.replace(first, end - first, replacement.str());
    Write(path + ".events", altered);
    Check(Replay(argv[1], path) != 0, "incorrect live contact digest detected");
    altered = original;
    const auto running = altered.find("S 0 1 1\n");
    Check(running != altered.npos, "running state fixture exists");
    altered.replace(running, 8, "S 0 1 0\n");
    Write(path + ".events", altered);
    Check(Replay(argv[1], path) != 0, "frame after panel-off cannot reactivate input");
    Write(path + ".events", original);
    std::ofstream extra(path, std::ios::binary | std::ios::app);
    extra.write(reinterpret_cast<const char*>(&event.packet), sizeof(event.packet));
    extra.close();
    Check(Replay(argv[1], path) != 0, "unpaired raw tail detected");
    // A process-local file size limit injects a real background write failure,
    // without filling a disk or requiring privileged mounts.
    const pid_t child = fork();
    Check(child >= 0, "fork write failure injection");
    if (!child) {
        signal(SIGXFSZ, SIG_IGN);
        rlimit limit{1024, 1024};
        if (setrlimit(RLIMIT_FSIZE, &limit)) _exit(2);
        himax::SessionRecorder writer(std::string(directory) + "/full");
        himax::SessionEvent begin;
        begin.kind = himax::SessionKind::Begin;
        std::memcpy(begin.project.data(), "W273AS2700", 11);
        if (!writer.Submit(begin) || !writer.Submit(event)) _exit(3);
        _exit(!writer.Close(true) && writer.Error() ? 0 : 4);
    }
    Check(Wait(child) == 0, "background write failure reported");
    {
        const std::string aborted = std::string(directory) + "/aborted";
        {
            himax::SessionRecorder writer(aborted);
            himax::SessionEvent begin;
            begin.kind = himax::SessionKind::Begin;
            std::memcpy(begin.project.data(), "W273AS2700", 11);
            Check(writer.Submit(begin) && writer.Submit(event), "queue before implicit abort");
        }
        Check(Replay(argv[1], aborted) != 0, "exception/early-return session marked incomplete");
    }
    std::filesystem::remove_all(directory);
    std::cout << "Normal capture, bounded queue and synchronized replay passed\n";
}
