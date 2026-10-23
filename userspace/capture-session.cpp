// SPDX-License-Identifier: MIT
#include "capture-session.h"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sstream>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>

namespace himax {

uint64_t ContactDigest(const Solvers::HeatmapFrame& frame) {
    uint64_t hash = 14695981039346656037ULL;
    const auto word = [&](uint32_t value) {
        for (unsigned int i = 0; i < 4; ++i) {
            hash = (hash ^ (value & 0xff)) * 1099511628211ULL;
            value >>= 8;
        }
    };
    word(frame.touch.output.contacts.size());
    for (const auto& c : frame.touch.output.contacts) {
        word(c.id);
        word(std::bit_cast<uint32_t>(c.x));
        word(std::bit_cast<uint32_t>(c.y));
        word(c.state);
        word(c.isReported);
        word(c.reportEvent);
        word(c.signalSum);
    }
    return hash;
}

bool SessionQueue::TryPush(const SessionEvent& event) {
    const auto head = m_head.load(std::memory_order_relaxed);
    if (head - m_tail.load(std::memory_order_acquire) >= kCapacity) return false;
    m_events[head % kCapacity] = event;
    m_head.store(head + 1, std::memory_order_release);
    return true;
}

bool SessionQueue::TryPop(SessionEvent& event) {
    const auto tail = m_tail.load(std::memory_order_relaxed);
    if (tail == m_head.load(std::memory_order_acquire)) return false;
    event = m_events[tail % kCapacity];
    m_tail.store(tail + 1, std::memory_order_release);
    return true;
}

struct SessionRecorder::Impl {
    std::ofstream raw, events;
    SessionQueue queue;
    std::atomic<int> error{0};
    std::atomic<uint64_t> dropped{0};
    std::atomic<bool> stopping{false};
    std::thread writer;
    int wake = -1;
    bool begun = false;
    SessionOrigin origin = SessionOrigin::Fresh;
    uint64_t frames = 0;

    void Wake() const {
        const uint64_t one = 1;
        // A full eventfd already represents a pending wake. The writer also
        // polls on a bounded timeout, so EINTR cannot strand queued records.
        (void)write(wake, &one, sizeof(one));
    }
    void Fail(int reason) {
        int expected = 0;
        error.compare_exchange_strong(expected, reason);
    }
    void Write(const SessionEvent& e) {
        const int reason = error.load();
        if (reason && reason != EOVERFLOW && reason != ECANCELED) return;
        switch (e.kind) {
        case SessionKind::Begin: {
            const auto end = std::find(e.project.begin(), e.project.end(), '\0');
            const std::string project(e.project.begin(), end);
            if (begun || !WriteHxstHeader(raw, project, e.coordinateFlags)) {
                Fail(EINVAL);
                return;
            }
            begun = true;
            events << "B " << project << ' ' << e.coordinateFlags << ' ' << e.observe << '\n';
            break;
        }
        case SessionKind::Frame:
            if (!begun || e.actionCount > e.actions.size()) { Fail(EINVAL); return; }
            raw.write(reinterpret_cast<const char*>(&e.packet), sizeof(e.packet));
            events << "F " << ++frames << ' ' << e.hostTimeNs << ' ' << e.rotation << ' '
                   << e.report << ' ' << e.status << ' ' << e.ctrl << ' ' << e.digest
                   << ' ' << e.actionCount;
            for (size_t i = 0; i < e.actionCount; ++i)
                events << ' ' << e.actions[i].action << ' ' << e.actions[i].parameter
                       << ' ' << e.actions[i].result;
            events << '\n';
            break;
        case SessionKind::State:
            events << "S " << e.hostTimeNs << ' ' << e.state.epoch << ' ' << e.state.running << '\n';
            break;
        case SessionKind::Release:
            events << "R " << e.hostTimeNs << '\n';
            break;
        case SessionKind::EndWindow:
            Fail(EINVAL); // LiveCapture consumes this control item, not a disk record.
            return;
        }
        if (!raw || !events) Fail(EIO);
    }
    void Run() {
        events << (origin == SessionOrigin::Fresh ? "HXSE 1\n" : "HXSE 2 live-no-checkpoint\n");
        for (;;) {
            SessionEvent event;
            while (queue.TryPop(event)) Write(event);
            // Read stopping before the final drain: producer publishes the
            // last record before Close's release-store.
            if (stopping.load(std::memory_order_acquire)) {
                while (queue.TryPop(event)) Write(event);
                break;
            }
            raw.flush();
            events.flush();
            if (!raw || !events) Fail(EIO);
            pollfd pending{wake, POLLIN, 0};
            (void)poll(&pending, 1, 100);
            uint64_t counter;
            (void)read(wake, &counter, sizeof(counter));
        }
        raw.flush();
        if (!raw) Fail(EIO);
        if (!begun || !frames) Fail(ENODATA);
        events << "END " << frames << ' ' << dropped.load() << ' ' << error.load() << '\n';
        events.flush();
        if (!events) Fail(EIO);
        raw.close();
        events.close();
        if (raw.fail() || events.fail()) Fail(EIO);
    }
};

SessionRecorder::SessionRecorder(const std::string& path, SessionOrigin origin)
    : m_impl(std::make_unique<Impl>()) {
    auto& s = *m_impl;
    s.origin = origin;
    s.raw.open(path, std::ios::binary | std::ios::out | std::ios::noreplace);
    if (!s.raw) { s.Fail(errno ? errno : EIO); return; }
    s.events.open(path + ".events", std::ios::out | std::ios::noreplace);
    if (!s.events) { s.Fail(errno ? errno : EIO); return; }
    s.wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (s.wake < 0) { s.Fail(errno); return; }
    try {
        s.writer = std::thread([&s] { s.Run(); });
    } catch (...) {
        s.Fail(EAGAIN);
    }
}

SessionRecorder::~SessionRecorder() {
    Close(false);
    if (m_impl->wake >= 0) close(m_impl->wake);
}

bool SessionRecorder::Submit(const SessionEvent& event) {
    auto& s = *m_impl;
    if (s.error.load() || s.stopping.load()) { ++s.dropped; return false; }
    if (!s.queue.TryPush(event)) {
        ++s.dropped;
        s.Fail(EOVERFLOW);
        return false;
    }
    s.Wake();
    return true;
}

bool SessionRecorder::Close(bool complete, uint64_t upstreamDropped) {
    auto& s = *m_impl;
    if (s.writer.joinable()) {
        if (upstreamDropped) {
            s.dropped.fetch_add(upstreamDropped);
            s.Fail(EOVERFLOW);
        }
        if (!complete) s.Fail(ECANCELED);
        s.stopping.store(true, std::memory_order_release);
        s.Wake();
        s.writer.join();
    }
    return s.error.load() == 0;
}

int SessionRecorder::Error() const { return m_impl->error.load(); }
uint64_t SessionRecorder::Dropped() const { return m_impl->dropped.load(); }

SessionReader::SessionReader(const char* path)
    : m_raw(path, RecordingFormat::Hxst, 0), m_events(std::string(path) + ".events") {
    if (!m_raw.Error().empty()) m_error = m_raw.Error();
    else if (m_raw.ProjectId().empty()) m_error = "Session requires HXRC panel header";
    std::array<char, 32> line{};
    if (m_error.empty()) {
        if (!m_events.getline(line.data(), line.size()))
            m_error = "Unsupported or missing HXSE sidecar";
        else if (std::string_view(line.data()) == "HXSE 2 live-no-checkpoint")
            m_error = "Live capture has no solver checkpoint; use raw --hxst replay, not --session";
        else if (std::string_view(line.data()) != "HXSE 1")
            m_error = "Unsupported or missing HXSE sidecar";
    }
}

bool SessionReader::Next(SessionEvent& e) {
    if (!m_error.empty() || m_ended) return false;
    std::array<char, 1024> line{};
    if (!m_events.getline(line.data(), line.size())) {
        m_error = "Missing END or oversized HXSE record";
        return false;
    }
    if (std::strlen(line.data()) + 1 != static_cast<size_t>(m_events.gcount())) {
        m_error = "Embedded NUL in HXSE record";
        return false;
    }
    std::istringstream in(line.data());
    std::string kind;
    e = {};
    in >> kind;
    bool valid = true;
    if (kind == "B") {
        e.kind = SessionKind::Begin;
        std::string project;
        int observe = 0;
        in >> project >> e.coordinateFlags >> observe;
        valid = !m_begun && project == m_raw.ProjectId() &&
                e.coordinateFlags == m_raw.CoordinateFlags() && (observe == 0 || observe == 1);
        if (valid) std::copy(project.begin(), project.end(), e.project.begin());
        e.observe = observe;
        m_begun = true;
    } else if (kind == "S") {
        e.kind = SessionKind::State;
        e.state.version = HX_RAW_ABI_VERSION;
        in >> e.hostTimeNs >> e.state.epoch >> e.state.running;
        valid = e.state.running <= 1;
    } else if (kind == "R") {
        e.kind = SessionKind::Release;
        in >> e.hostTimeNs;
    } else if (kind == "F") {
        e.kind = SessionKind::Frame;
        uint64_t index = 0;
        int report = 0;
        in >> index >> e.hostTimeNs >> e.rotation >> report >> e.status >> e.ctrl
           >> e.digest >> e.actionCount;
        valid = m_begun && index == ++m_frames && e.rotation >= -1 && e.rotation <= 3 &&
                (report == 0 || report == 1) && e.actionCount <= e.actions.size();
        e.report = report;
        if (valid) for (size_t i = 0; i < e.actionCount; ++i) {
            auto& a = e.actions[i];
            in >> a.action >> a.parameter >> a.result;
            valid &= a.action <= HX_RAW_RESET && a.parameter <= 255 &&
                     a.result >= -1 && a.result <= 4095;
        }
        RecordedFrame raw;
        valid &= m_raw.Next(raw);
        if (valid) {
            e.packet.size = sizeof(e.packet);
            e.packet.version = HX_RAW_ABI_VERSION;
            e.packet.epoch = raw.epoch;
            e.packet.sequence = raw.sequence;
            e.packet.timestamp_ns = raw.timeNs;
            e.packet.flags = (raw.sos ? HX_RAW_FRAME_SOS : 0) | (raw.charger ? HX_RAW_FRAME_CHARGER : 0);
            e.packet.payload_size = raw.length;
            std::copy(raw.payload.begin(), raw.payload.end(), e.packet.payload);
        }
    } else if (kind == "END") {
        uint64_t count = 0, dropped = 0;
        int error = 0;
        in >> count >> dropped >> error;
        RecordedFrame extra;
        valid = m_begun && count == m_frames && count && !dropped && !error &&
                !m_raw.Next(extra) && m_raw.Error().empty() &&
                m_events.peek() == std::char_traits<char>::eof();
        m_ended = true;
    } else {
        valid = false;
    }
    if (!in || !valid || (in >> std::ws, !in.eof())) {
        m_error = "Malformed, incomplete or inconsistent HXSE session";
        return false;
    }
    return !m_ended;
}

}  // namespace himax
