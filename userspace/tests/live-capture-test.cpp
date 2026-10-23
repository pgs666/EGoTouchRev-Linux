// SPDX-License-Identifier: MIT
#include "live-capture.h"
#include "session-replay.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

static void Check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

template<class Predicate> static void Wait(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Check(predicate(), "background writer reached expected state within deadline");
}

static std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

static std::vector<std::filesystem::path> Captures(const std::string& directory) {
    std::vector<std::filesystem::path> result;
    for (const auto& child : std::filesystem::directory_iterator(directory)) {
        const auto path = child.path() / "capture.hxst";
        if (std::filesystem::exists(path)) result.push_back(path);
    }
    std::sort(result.begin(), result.end());
    return result;
}

int main() {
    char temporary[] = "/tmp/himax-live-test-XXXXXX";
    Check(mkdtemp(temporary), "private fixture root");
    const std::string directory = temporary;
    himax::SessionEvent begin;
    begin.kind = himax::SessionKind::Begin;
    constexpr std::string_view project = "W273AS2700";
    std::copy(project.begin(), project.end(), begin.project.begin());
    begin.coordinateFlags = 5;
    himax::SessionEvent event;
    event.kind = himax::SessionKind::Frame;
    event.packet.size = sizeof(event.packet);
    event.packet.version = HX_RAW_ABI_VERSION;
    event.packet.payload_size = HX_RAW_MASTER_SIZE;
    event.packet.epoch = 40;
    event.packet.sequence = 90;
    event.packet.timestamp_ns = 1000000000;
    event.packet.payload[40] = 123;
    event.rotation = 3;
    event.actions[0] = {HX_RAW_CLEAR_STATUS, 8, 0};
    event.actionCount = 1;
    {
        himax::LiveCapture live(directory, 1);
        Check(!live.Error() && !live.Busy() && !live.Recording(), "disabled window creates no files");
        live.Tick(100);
        Check(Captures(directory).empty() && !live.Submit(event), "idle path performs no recording");
        Check(!live.Start(event, 100) && !live.Start(begin, UINT64_MAX), "invalid begin/time rejected");
        Check(live.Start(begin, 1000000000), "start without touching hardware or solver");
        Check(!live.Start(begin, 1000000000), "overlapping requests rejected");
        Check(live.Submit(event), "active frame copied to nonblocking queue");
        auto state = event;
        state.kind = himax::SessionKind::State;
        state.state = {HX_RAW_ABI_VERSION, 0, 41};
        Check(live.Submit(state), "running capture keeps lifecycle evidence");
        live.Tick(1999999999);
        Check(live.Recording(), "window remains active immediately before deadline");
        live.Tick(2000000000);
        Check(!live.Recording() && !live.Submit(event), "deadline closes producer without I/O wait");
        Wait([&] { return !live.Busy(); });
        Check(!live.Error(), "complete live window closed successfully");
        const auto first = Captures(directory);
        Check(first.size() == 1, "one private capture directory");
        const auto path = first.front().string();
        const auto sidecar = Read(path + ".events");
        Check(sidecar.starts_with("HXSE 4 live-no-checkpoint\n") &&
              sidecar.ends_with("END 1 0 0\n"), "live origin is explicit, completed window has clean footer");
        himax::RecordingReader reader(path.c_str(), himax::RecordingFormat::Hxst, 0);
        himax::RecordedFrame frame;
        Check(reader.ProjectId() == project && reader.CoordinateFlags() == 5 && reader.Next(frame) &&
              frame.epoch == 40 && frame.sequence == 90 && frame.payload[40] == 123 &&
              frame.length == HX_RAW_MASTER_SIZE && !reader.Next(frame) && reader.Error().empty(),
              "raw evidence/identity/epoch preserved without a new AFE session");
        himax::SessionReader session(path.c_str());
        Check(session.Error().find("no solver checkpoint") != std::string::npos &&
              himax::ReplaySession(path.c_str()) == 1,
              "partial live state cannot masquerade as strictly reproducible solver session");
        Check(live.Start(begin, 3000000000) && live.Submit(event), "new window after previous writer drained");
        live.Stop();
        live.Tick(3000000001);
        Wait([&] { return !live.Busy(); });
        Check(!live.Error() && Captures(directory).size() == 2 &&
              Read(path + ".events") == sidecar, "manual stop saves separately without overwrite");
        Check(live.Start(begin, 4000000000) && live.Submit(event), "third window");
        Check(!live.Submit(begin) && live.Error() == EINVAL, "control misuse disables window, not input");
        live.Tick(4000000001);
        Wait([&] { return !live.Busy(); });
        Check(live.Error() == EINVAL, "failed capture stays failed after drain");
        Check(live.Start(begin, 5000000000), "subsequent capture clears previous error");
        Check(live.Submit(event), "recovered capture accepts events");
        live.Stop();
        live.Tick(5000000001);
        Wait([&] { return !live.Busy(); });
        Check(!live.Error(), "error recovery does not poison next window");
    }
    {
        himax::LiveCapture live(directory, 1);
        Check(live.Start(begin, 1) && live.Submit(event), "capture active at daemon shutdown");
    }
    unsigned clean = 0, cancelled = 0;
    for (const auto& path : Captures(directory)) {
        const auto text = Read(path.string() + ".events");
        clean += text.ends_with("END 1 0 0\n");
        cancelled += text.find("END 1 0 " + std::to_string(ECANCELED) + "\n") != std::string::npos;
    }
    Check(clean == 3 && cancelled == 2, "failed and interrupted windows never get successful footers");
    const std::string overflowPath = directory + "/overflow.hxst";
    {
        himax::SessionRecorder writer(overflowPath, himax::SessionOrigin::Live);
        Check(writer.Submit(begin) && writer.Submit(event), "queued prefix before upstream overflow");
        Check(!writer.Close(true, 3) && writer.Error() == EOVERFLOW && writer.Dropped() == 3,
              "upstream queue drops remain visible in persisted failure status");
    }
    Check(Read(overflowPath + ".events").ends_with("END 1 3 " + std::to_string(EOVERFLOW) + "\n"),
          "overflow footer cannot claim clean lossless capture");
    const std::string emptyDirectory = directory + "/empty-window";
    Check(mkdir(emptyDirectory.c_str(), 0700) == 0, "empty-window fixture");
    {
        himax::LiveCapture empty(emptyDirectory, 1);
        Check(empty.Start(begin, 1), "request window with no frames");
        empty.Stop();
        empty.Tick(2);
        Wait([&] { return !empty.Busy(); });
        Check(empty.Error() == ENODATA, "window with no raw frames cannot report success");
    }
    const auto checkFailure = [&](const std::string& path, int expected) {
        himax::LiveCapture live(path, 1);
        Check(live.Start(begin, 1), "invalid directory validation stays off producer thread");
        Wait([&] { return live.Error() != 0; });
        if (expected) Check(live.Error() == expected, "expected directory failure");
        live.Tick(2);
        Wait([&] { return !live.Busy(); });
        Check(!live.Recording(), "directory failure cannot leave active capture");
    };
    checkFailure(directory + "/missing", ENOENT);
    Check(symlink(directory.c_str(), (directory + "/link").c_str()) == 0, "symlink fixture");
    checkFailure(directory + "/link", 0);
    Check(unlink((directory + "/link").c_str()) == 0 && chmod(directory.c_str(), 0777) == 0,
          "untrusted directory fixture");
    checkFailure(directory, EACCES);
    Check(chmod(directory.c_str(), 0700) == 0, "restore private fixture mode");
    const std::string diskFailure = directory + "/disk-failure";
    Check(mkdir(diskFailure.c_str(), 0700) == 0, "private write-failure fixture");
    const pid_t child = fork();
    Check(child >= 0, "fork isolated file-size failure");
    if (!child) {
        signal(SIGXFSZ, SIG_IGN);
        rlimit limit{1024, 1024};
        if (setrlimit(RLIMIT_FSIZE, &limit)) _exit(2);
        bool failed = false;
        {
            himax::LiveCapture live(diskFailure, 1);
            if (!live.Start(begin, 1) || !live.Submit(event)) _exit(3);
            live.Stop();
            live.Tick(2);
            Wait([&] { return !live.Busy(); });
            failed = live.Error() != 0;
        }
        _exit(failed ? 0 : 4);
    }
    int status;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    Check(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "real background write failure is isolated and reported");
    for (unsigned seconds : {0u, 121u, UINT32_MAX}) {
        himax::LiveCapture invalid(directory, seconds);
        Check(invalid.Error() == EINVAL && !invalid.Start(begin, 1), "invalid duration rejected");
    }
    std::filesystem::remove_all(directory);
    std::cout << "Live capture: no-reset raw windows, deadlines, manual stop, paired evidence, "
                 "checkpoint rejection, write failure, restart and private paths passed\n";
}
