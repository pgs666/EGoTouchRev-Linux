// SPDX-License-Identifier: MIT
// Execute actual touchd main, solver, capture recorder and strict replay.
// Replace only hardware/input/desktop boundaries and the main-thread clock.
#include "v4l2-capture.h"
#include "input.h"
#include "screen-rotation.h"
#include "session-replay.h"
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iostream>
#include <poll.h>
#include <pthread.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>

int himax_touchd_main(int, char**);
extern "C" int __real_poll(pollfd*, nfds_t, int);
extern "C" int __real_clock_gettime(clockid_t, timespec*);

namespace {
enum class Scenario { Healthy, Accepted, NoFirst, Races, FirstBad, KickExhausted,
                      Unsupported, NeverReady, Invalid, Default };
Scenario scenario;
pthread_t mainThread;
bool virtualClock = false, active = false;
bool fatalRequestReturned = false;
uint64_t nowNs = 1000000000, epoch = 1, sequence = 0, nextFrame = 1000000000;
unsigned initial = 0, following = 0, requests = 0, accepted = 0, resets = 0, reports = 0, releases = 0;
unsigned starts = 0, opens = 0;
unsigned kicks = 0;
bool oldBuffered = false;
uint64_t oldEpoch = 0, oldSequence = 0;
unsigned oldDiscarded = 0;
std::deque<hx_raw_state> states;
void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
bool First() { return scenario != Scenario::NoFirst && initial < 100; }
bool Following() {
    return accepted && scenario != Scenario::NeverReady &&
           (scenario == Scenario::Invalid || following < 100);
}
bool Data() { return oldBuffered || First() || Following(); }
}

extern "C" int __wrap_clock_gettime(clockid_t clock, timespec* value) {
    if (virtualClock && clock == CLOCK_MONOTONIC && pthread_equal(pthread_self(), mainThread)) {
        value->tv_sec = nowNs / 1000000000;
        value->tv_nsec = nowNs % 1000000000;
        return 0;
    }
    return __real_clock_gettime(clock, value);
}
extern "C" int __wrap_poll(pollfd* fds, nfds_t count, int timeout) {
    if (count != 1 || fds[0].fd != 91) return __real_poll(fds, count, timeout);
    Check(!fatalRequestReturned, "fatal restart rejection exits before polling or processing more data");
    fds[0].revents = 0;
    if (!states.empty()) { fds[0].revents = POLLPRI; return 1; }
    if (Data()) {
        nowNs = std::max(nowNs + 9000000, nextFrame);
        fds[0].revents = POLLIN;
        return 1;
    }
    if ((scenario == Scenario::Healthy && initial == 100) ||
        (accepted && following == 100 && scenario != Scenario::Invalid)) {
        raise(SIGTERM);
        return 0;
    }
    nowNs += uint64_t(timeout) * 1000000;
    return 0;
}

namespace himax {
Capture::~Capture() = default;
bool Capture::Open(const std::string& path, bool) {
    Check(path == "/offline-himax", "only fake device requested");
    ++opens;
    m_path = path;
    m_fd = 91;
    return RefreshInfo();
}
bool Capture::RefreshInfo() {
    m_info = {};
    m_info.size = sizeof(m_info);
    m_info.version = HX_RAW_ABI_VERSION;
    m_info.rows = 40;
    m_info.cols = 60;
    m_info.payload_size = HX_RAW_MASTER_SIZE;
    std::copy_n("W273AS2700", 11, m_info.project_id);
    return true;
}
bool Capture::Start() {
    ++starts;
    states.push_back({HX_RAW_ABI_VERSION, 0, epoch});
    states.push_back({HX_RAW_ABI_VERSION, 1, epoch});
    return true;
}
bool Capture::Event(hx_raw_state& state) {
    if (states.empty()) { errno = EAGAIN; return false; }
    state = states.front();
    states.pop_front();
    return true;
}
bool Capture::Frame(hx_raw_frame& packet) {
    Check(Data(), "poll only advertises available data");
    packet = {};
    packet.size = sizeof(packet);
    packet.version = HX_RAW_ABI_VERSION;
    if (oldBuffered) {
        oldBuffered = false;
        ++oldDiscarded;
        packet.epoch = oldEpoch;
        packet.sequence = oldSequence;
        packet.timestamp_ns = nowNs;
        packet.payload_size = HX_RAW_MASTER_SIZE;
        return true; // Must be fenced before even attempting raw conversion.
    }
    packet.epoch = epoch;
    packet.sequence = ++sequence;
    packet.timestamp_ns = nowNs;
    packet.payload_size = HX_RAW_MASTER_SIZE;
    const unsigned index = accepted ? following++ : initial++;
    packet.flags = accepted ? HX_RAW_FRAME_SOS : 0;
    packet.payload[0] = 0x5a;
    packet.payload[1] = 0xa5;
    for (size_t node = 0; node < 2400; ++node) {
        const auto row = node / 60, col = node % 60;
        const bool finger = index >= 30 && (!accepted || index < 80) &&
            row >= 19 && row <= 21 && col >= 29 && col <= 31;
        const uint16_t value = 0x7ffe + (finger ? (row == 20 && col == 30 ? 1000 : 500) : 0);
        packet.payload[4 + 2 * node] = value & 0xff;
        packet.payload[5 + 2 * node] = value >> 8;
    }
    packet.payload[4804 + 6] = index < 5 ? 0xbb : 0;
    uint32_t sum = 0;
    for (size_t j = 4; j < HX_RAW_MASTER_SIZE; j += 2)
        sum += packet.payload[j] | packet.payload[j + 1] << 8;
    const uint16_t checksum = uint16_t(0u - sum);
    packet.payload[2] = checksum & 0xff;
    packet.payload[3] = checksum >> 8;
    if (accepted && (scenario == Scenario::Invalid || (scenario == Scenario::FirstBad && index < 5)))
        packet.payload[2] ^= 1;
    return true;
}
bool Capture::Command(uint64_t requestEpoch, uint64_t requestSequence, hx_raw_action action, uint32_t) {
    Check(requestEpoch == epoch && requestSequence == sequence, "frame command uses current producer token");
    if (action == HX_RAW_RESET) ++resets;
    return true;
}
bool Capture::Rearm(uint64_t, uint64_t) {
    Check(scenario == Scenario::KickExhausted, "restart option does not implicitly enable producer rearm");
    ++kicks;
    errno = EIO;
    return false;
}
bool Capture::Restart(uint64_t requestEpoch, uint64_t requestSequence) {
    ++requests;
    Check(requestEpoch == epoch && requestSequence == sequence, "actual daemon uses exact stall token");
    Check(initial == 100 || scenario == Scenario::NoFirst, "no restart before producer stalls");
    if (scenario == Scenario::Unsupported) {
        fatalRequestReturned = true;
        errno = ENOTTY;
        return false;
    }
    if (scenario == Scenario::Races && requests <= 2) { errno = EAGAIN; return false; }
    ++accepted;
    oldEpoch = epoch;
    oldSequence = sequence + 1;
    oldBuffered = true;
    // A ready event queued before this request must not reopen the old epoch.
    states.push_back({HX_RAW_ABI_VERSION, 1, epoch});
    states.push_back({HX_RAW_ABI_VERSION, 0, epoch});
    if (scenario != Scenario::NeverReady) {
        ++epoch;
        sequence = 0;
        states.push_back({HX_RAW_ABI_VERSION, 0, epoch});
        states.push_back({HX_RAW_ABI_VERSION, 1, epoch});
        nextFrame = nowNs + 150000000;
    }
    return true;
}
Input::~Input() = default;
bool Input::Open(uint32_t) { return true; }
bool Input::Report(std::span<const Contact> contacts, uint64_t) {
    if (!contacts.empty()) ++reports;
    else if (active) ++releases;
    active = !contacts.empty();
    return true;
}
bool SlotState::Active() const { return active; }
ScreenRotationProvider::ScreenRotationProvider(std::string, std::string_view, std::string_view) {}
ScreenRotationProvider::~ScreenRotationProvider() = default;
void ScreenRotationProvider::SetFixed(std::optional<int> value) noexcept { m_fixed = value; }
bool ScreenRotationProvider::RequestInitial() const { return false; }
std::optional<ScreenRotationSample> ScreenRotationProvider::Poll() const { return std::nullopt; }
std::optional<int> ParseScreenRotation(std::string_view) noexcept { return std::nullopt; }
} // namespace himax

static void Run(Scenario test, const std::string& path) {
    scenario = test;
    mainThread = pthread_self();
    unsetenv("NOTIFY_SOCKET");
    const bool enabled = test != Scenario::Default;
    const bool record = test == Scenario::Accepted || test == Scenario::NoFirst ||
                        test == Scenario::Races || test == Scenario::FirstBad || test == Scenario::KickExhausted;
    std::vector<std::string> args{"himax-touchd", "--device", "/offline-himax"};
    if (enabled) args.emplace_back("--experimental-restart");
    if (test == Scenario::KickExhausted) args.emplace_back("--experimental-rearm");
    if (record) { args.emplace_back("--record"); args.push_back(path); }
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    virtualClock = true;
    const int result = himax_touchd_main(args.size(), argv.data());
    virtualClock = false;
    const bool failed = test == Scenario::Unsupported || test == Scenario::NeverReady ||
                        test == Scenario::Invalid || test == Scenario::Default;
    Check(result == int(failed), "actual daemon exit code matches recovery outcome");
    Check(opens == 1 && starts == 1 && resets == 0,
          "same Capture/stream throughout; no supervisor emulation or legacy reset during reconstruction");
    Check(!active, "input always released before daemon exit");
    Check(kicks == (test == Scenario::KickExhausted ? 7u : 0u),
          "both experimental options exhaust seven no-reset attempts before in-process reconstruction");
    if (test == Scenario::Healthy || test == Scenario::Default)
        Check(!requests && !accepted, "healthy and default modes issue no new restart ioctl");
    else if (test == Scenario::Unsupported)
        Check(requests == 1 && !accepted, "old module falls back once without reset loop");
    else
        Check(requests == (test == Scenario::Races ? 3u : 1u) && accepted == 1,
              "transient race retries bounded; accepted restart never repeated during wait");
    if (accepted)
        Check(oldDiscarded == 1, "actual daemon drains/fences old buffered packet and late ready event");
    if (test == Scenario::Invalid)
        Check(following > 5 && nowNs < 23000000000ULL,
              "bad new frames cannot complete recovery or trigger nested frame resets; timeout bounded");
    if (!failed) Check(reports > 20 && releases >= 1, "real TSACore report path active and releases input");
    if (record) Check(himax::ReplaySession(path.c_str()) == 0,
                      "actual daemon-produced recording passes strict replay after same-process reconstruction");
}

int main() {
    char temporary[] = "/tmp/himax-daemon-restart-XXXXXX";
    Check(mkdtemp(temporary), "private test directory");
    for (auto test : {Scenario::Healthy, Scenario::Accepted, Scenario::NoFirst, Scenario::Races,
                      Scenario::FirstBad, Scenario::KickExhausted,
                      Scenario::Unsupported, Scenario::NeverReady, Scenario::Invalid, Scenario::Default}) {
        const auto child = fork();
        Check(child >= 0, "independent daemon signal and core state");
        if (!child) {
            Run(test, std::string(temporary) + "/" + std::to_string(int(test)) + ".hxst");
            std::exit(0);
        }
        int status = 0;
        pid_t result;
        do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
        Check(result == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "offline actual-daemon scenario passed");
    }
    std::filesystem::remove_all(temporary);
    std::cout << "Actual daemon same-process AFE recovery, default/old-module/deadline gates and recorded replay passed\n";
}
