// SPDX-License-Identifier: MIT
#include "afe-action.h"
#include "capture-session.h"
#include "solver-screen.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
static int Wait(pid_t child) {
    int status = 0;
    pid_t result;
    do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
    Check(result == child && WIFEXITED(status), "child exits normally");
    return WEXITSTATUS(status);
}
static int Replay(const char* program, const std::string& path) {
    const auto child = fork();
    Check(child >= 0, "fork strict replay");
    if (!child) {
        int quiet = open("/dev/null", O_WRONLY);
        if (quiet < 0 || dup2(quiet, STDOUT_FILENO) < 0) _exit(127);
        close(quiet);
        execl(program, program, "--session", path.c_str(), nullptr);
        _exit(127);
    }
    return Wait(child);
}
static std::string Read(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
static void Write(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    Check(out.good(), "write corrupted evidence fixture");
}
static void Fixture(const std::string& path, bool noFirstFrame) {
    himax::SessionRecorder writer(path);
    Check(!writer.Error(), "create session");
    auto solver = std::make_unique<Solvers::Tsa::TsaCoreSolver>();
    Check(solver->SetPanelProjectId("W273AS2700"), "fixed panel profile");
    auto frame = std::make_unique<Solvers::HeatmapFrame>();
    auto event = std::make_unique<himax::SessionEvent>();
    himax::SolverScreenState screen;
    std::array<uint8_t, Frame::kMasterFrameSize> thp{};
    event->kind = himax::SessionKind::Begin;
    std::copy_n("W273AS2700", 11, event->project.begin());
    event->restartEnabled = true;
    Check(writer.Submit(*event), "record experimental policy identity");
    uint64_t time = 1000000000, epoch = 1, sequence = 0;
    const auto state = [&](bool running, uint64_t when) {
        himax::SessionEvent e;
        e.kind = himax::SessionKind::State;
        e.hostTimeNs = when;
        e.state = {HX_RAW_ABI_VERSION, uint32_t(running), epoch};
        Check(writer.Submit(e), "record lifecycle");
    };
    state(true, time);
    solver->OnAfeStarted();
    unsigned contactsBefore = 0, contactsAfter = 0;
    for (unsigned i = 0; i < 160; ++i) {
        if (i == (noFirstFrame ? 0u : 70u)) {
            *event = {};
            event->kind = himax::SessionKind::Restart;
            event->hostTimeNs = time + 4001000000ULL;
            event->restart = {event->hostTimeNs + 1000000, epoch, sequence, 0};
            Check(writer.Submit(*event), "record no-frame-triggered reconstruction");
            solver->OnAfeSos();
            solver->OnAfeStopped();
            time = event->restart.completedNs;
            state(false, time);
            ++epoch;
            sequence = 0;
            state(false, time + 1000000);
            state(true, time + 100000000);
            time += 150000000;
            solver->OnAfeStopped();
            solver->OnAfeSos();
            solver->OnAfeStarted();
        }
        *event = {};
        auto& p = event->packet;
        p.size = sizeof(p);
        p.version = HX_RAW_ABI_VERSION;
        p.epoch = epoch;
        p.sequence = ++sequence;
        p.timestamp_ns = time;
        p.payload_size = HX_RAW_MASTER_SIZE;
        p.payload[0] = 0x5a;
        p.payload[1] = 0xa5;
        p.flags = epoch == 2 ? HX_RAW_FRAME_SOS : 0;
        for (size_t node = 0; node < 2400; ++node) {
            const auto row = node / 60, col = node % 60;
            const bool finger = ((i >= 20 && i < 70) || (i >= 100 && i < 140)) &&
                row >= 19 && row <= 21 && col >= 29 && col <= 31;
            const uint16_t value = 0x7ffe + (finger ? (row == 20 && col == 30 ? 1000 : 500) : 0);
            p.payload[4 + 2 * node] = value & 0xff;
            p.payload[5 + 2 * node] = value >> 8;
        }
        p.payload[4804 + 6] = (i < 5 || (!noFirstFrame && i >= 70 && i < 75)) ? 0xbb : 0;
        uint32_t sum = 0;
        for (size_t j = 4; j < HX_RAW_MASTER_SIZE; j += 2)
            sum += p.payload[j] | p.payload[j + 1] << 8;
        const uint16_t checksum = uint16_t(0u - sum);
        p.payload[2] = checksum & 0xff;
        p.payload[3] = checksum >> 8;
        event->hostTimeNs = time + 1000000;
        Check(himax::ToThpMaster({p.payload, p.payload_size}, thp) == himax::FrameError::None,
              "synthetic mutual frame is checksum valid");
        frame->rawPtr = thp.data();
        frame->rawLen = thp.size();
        event->report = screen.Process(*solver, *frame, time / 1000000);
        event->status = solver->LastAfeStatus();
        event->ctrl = solver->LastCtrlFlags();
        event->digest = himax::ContactDigest(*frame);
        if (epoch == 1) contactsBefore += event->report;
        else contactsAfter += event->report;
        for (const auto& action : solver->PendingAfeActions()) {
            event->actions[event->actionCount++] = {himax::HardwareAction(action.type), action.param, 0};
            solver->OnAfeActionExecuted(action, true);
        }
        Check(writer.Submit(*event), "frame and original solver results recorded");
        time += 9000000;
    }
    if (!(noFirstFrame || contactsBefore > 10) || contactsAfter <= 10)
        std::cerr << "fixture contacts before=" << contactsBefore << " after=" << contactsAfter << '\n';
    Check((noFirstFrame || contactsBefore > 10) && contactsAfter > 10,
          "fixture exercises real mapper contacts surrounding same-object reconstruction");
    Check(writer.Close(true), "recording complete");
}

int main(int argc, char** argv) {
    Check(argc == 2, "replay binary supplied");
    char directory[] = "/tmp/himax-restart-session-XXXXXX";
    Check(mkdtemp(directory), "private fixture directory");
    for (bool noFirst : {false, true}) {
        const auto path = std::string(directory) + (noFirst ? "/no-first.hxst" : "/mid-touch.hxst");
        const auto child = fork();
        Check(child >= 0, "independent TSACore globals per fixture");
        if (!child) { Fixture(path, noFirst); std::exit(0); }
        Check(Wait(child) == 0 && Replay(argv[1], path) == 0,
              "strict replay preserves real solver output across reconstructed epochs");
        const auto original = Read(path + ".events");
        const auto first = original.find("J ");
        const auto end = original.find('\n', first);
        Check(first != original.npos && end != original.npos, "standalone restart request present");
        const auto line = original.substr(first, end - first);
        for (unsigned mutation = 0; mutation < 7; ++mutation) {
            auto altered = original;
            std::istringstream in(line);
            std::string kind;
            uint64_t began, completed, epoch, sequence;
            int result;
            in >> kind >> began >> completed >> epoch >> sequence >> result;
            if (mutation == 0) ++epoch;
            if (mutation == 1) ++sequence;
            if (mutation == 2) completed = began - 1;
            if (mutation == 3) result = -1;
            if (mutation == 4) result = 4096;
            if (mutation == 5) completed += 20000000000ULL;
            if (mutation == 6) began = completed + 1;
            std::ostringstream out;
            out << "J " << began << ' ' << completed << ' ' << epoch << ' ' << sequence << ' ' << result;
            altered.replace(first, end - first, out.str());
            Write(path + ".events", altered);
            Check(Replay(argv[1], path) != 0, "invalid restart token/result/timeline cannot verify");
        }
        {
            auto altered = original;
            const auto begin = altered.find("B W273AS2700 0 0 1\n");
            Check(begin != altered.npos, "restart policy metadata present");
            altered.replace(begin, 18, "B W273AS2700 0 0 0\n");
            Write(path + ".events", altered);
            Check(Replay(argv[1], path) != 0, "restart without explicit experimental policy rejected");
        }
        Write(path + ".events", original);
        Check(Replay(argv[1], path) == 0, "restored original still verifies");
    }
    std::filesystem::remove_all(directory);
    std::cout << "No-first-frame and active-contact same-core reconstruction strict replay and corruption checks passed\n";
}
