// SPDX-License-Identifier: MIT
#include "live-capture.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace himax {

LiveCapture::LiveCapture(std::string directory, unsigned seconds)
    : m_directory(std::move(directory)), m_seconds(seconds) {
    if (!seconds || seconds > 120 || m_directory.empty()) { Fail(EINVAL); return; }
    m_wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m_wake < 0) { Fail(errno); return; }
    try { m_worker = std::thread([this] { Run(); }); }
    catch (...) { Fail(EAGAIN); }
}

LiveCapture::~LiveCapture() {
    m_stopping.store(true, std::memory_order_release);
    Wake();
    if (m_worker.joinable()) m_worker.join();
    if (m_wake >= 0) close(m_wake);
}

void LiveCapture::Fail(int error) {
    int expected = 0;
    m_error.compare_exchange_strong(expected, error);
}

void LiveCapture::Wake() const {
    if (m_wake < 0) return;
    const uint64_t one = 1;
    (void)write(m_wake, &one, sizeof(one));
}

bool LiveCapture::Start(const SessionEvent& begin, uint64_t now) {
    if (!m_worker.joinable() || begin.kind != SessionKind::Begin || m_endPending ||
        now > UINT64_MAX - uint64_t(m_seconds) * 1000000000ULL) return false;
    bool idle = false;
    if (!m_busy.compare_exchange_strong(idle, true, std::memory_order_acq_rel)) return false;
    m_error.store(0);
    m_dropped.store(0);
    m_deadline = now + uint64_t(m_seconds) * 1000000000ULL;
    m_recording = true;
    // The previous window releases busy only after consuming its END item.
    if (!m_queue.TryPush(begin)) {
        Fail(EOVERFLOW);
        m_recording = false;
        m_busy.store(false, std::memory_order_release);
        return false;
    }
    Wake();
    return true;
}

void LiveCapture::Stop() {
    if (!m_recording) return;
    m_recording = false;
    m_endPending = true;
}

void LiveCapture::Tick(uint64_t now) {
    if (m_recording && (now >= m_deadline || m_error.load())) Stop();
    if (m_endPending) {
        SessionEvent end;
        end.kind = SessionKind::EndWindow;
        // A full queue must not block input. Retry the terminal marker at the
        // next loop tick; do not reopen until the worker has closed this file.
        if (m_queue.TryPush(end)) {
            m_endPending = false;
            Wake();
        }
    }
}

bool LiveCapture::Submit(const SessionEvent& event) {
    if (!Recording()) return false;
    if (event.kind == SessionKind::Begin || event.kind == SessionKind::EndWindow) {
        Fail(EINVAL);
        return false;
    }
    if (!m_queue.TryPush(event)) {
        ++m_dropped;
        Fail(EOVERFLOW);
        return false;
    }
    Wake();
    return true;
}

void LiveCapture::Run() {
    std::unique_ptr<SessionRecorder> recorder;
    std::string path;
    const auto finish = [&](bool complete) {
        const bool ok = recorder && recorder->Close(complete && !m_error.load(), m_dropped.load());
        if (!ok) Fail(recorder ? recorder->Error() : ENODATA);
        std::cerr << "Live capture " << (ok ? "window saved" : "incomplete")
                  << ": " << path << " dropped=" << m_dropped.load()
                  << " error=" << m_error.load() << " (no solver checkpoint)\n";
        recorder.reset();
        m_busy.store(false, std::memory_order_release);
    };
    const auto consume = [&](const SessionEvent& event) {
        if (event.kind == SessionKind::EndWindow) { finish(true); return; }
        if (event.kind == SessionKind::Begin) {
            path.clear();
            // Only the worker touches directories. Require a private directory
            // owned by the service; never follow an untrusted final symlink.
            const int fd = open(m_directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            struct stat info {};
            if (fd < 0) { Fail(errno); return; }
            const bool safe = fstat(fd, &info) == 0 && info.st_uid == geteuid() && !(info.st_mode & 0022);
            close(fd);
            if (!safe) { Fail(EACCES); return; }
            const std::string pattern = m_directory + "/live-XXXXXXXX";
            std::vector<char> name(pattern.begin(), pattern.end());
            name.push_back(0);
            if (!mkdtemp(name.data())) { Fail(errno); return; }
            path = std::string(name.data()) + "/capture.hxst";
            recorder = std::make_unique<SessionRecorder>(path, SessionOrigin::Live);
            if (recorder->Error()) { Fail(recorder->Error()); return; }
            std::cerr << "Live capture started: " << path << " (no reset/checkpoint)\n";
        }
        if (recorder && !recorder->Submit(event)) Fail(recorder->Error());
    };
    const auto safelyConsume = [&](const SessionEvent& event) {
        try { consume(event); }
        catch (...) { Fail(ENOMEM); }
    };
    for (;;) {
        SessionEvent event;
        while (m_queue.TryPop(event)) safelyConsume(event);
        if (m_stopping.load(std::memory_order_acquire)) {
            while (m_queue.TryPop(event)) safelyConsume(event);
            if (m_busy.load()) finish(false);
            break;
        }
        pollfd pending{m_wake, POLLIN, 0};
        (void)poll(&pending, 1, 100);
        uint64_t counter;
        (void)read(m_wake, &counter, sizeof(counter));
    }
}

} // namespace himax
