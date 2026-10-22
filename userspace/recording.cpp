// SPDX-License-Identifier: MIT
#include "recording.h"
#include "hx-v4l2-uapi.h"

#include <algorithm>
#include <cstring>

namespace himax {
namespace {
uint64_t Le(const uint8_t* p, unsigned int bytes) {
    uint64_t result = 0;
    for (unsigned int i = 0; i < bytes; ++i)
        result |= static_cast<uint64_t>(p[i]) << (i * 8);
    return result;
}
}  // namespace

RecordingReader::RecordingReader(const char* path, RecordingFormat format, size_t nativeBytes)
    : m_input(path, std::ios::binary), m_format(format) {
    if (!m_input) {
        m_error = "Cannot open recording";
        return;
    }
    if (format == RecordingFormat::Trace) {
        std::array<uint8_t, 40> header{};
        if (!m_input.read(reinterpret_cast<char*>(header.data()), header.size()) ||
            Le(header.data(), 4) != 0x52545848 || Le(header.data() + 4, 2) < 1 ||
            Le(header.data() + 4, 2) > 3 || Le(header.data() + 6, 2) != header.size()) {
            m_error = "Unsupported or truncated HXTR header";
            return;
        }
        const auto size = Le(header.data() + 8, 4);
        m_traceCount = Le(header.data() + 16, 4);
        if (size < kNativeTransportSize + 24 || size > 1024 * 1024 ||
            m_traceCount > Le(header.data() + 12, 4) || m_traceCount > 4096) {
            m_error = "Invalid HXTR dimensions";
            return;
        }
        m_buffer.resize(size);
    } else if (format == RecordingFormat::Hxst) {
        m_buffer.resize(sizeof(hx_raw_frame));
    } else if (nativeBytes == kNativeTransportSize || nativeBytes == kNativeMasterSize) {
        m_buffer.resize(nativeBytes);
    } else {
        m_error = "Invalid native record size";
    }
}

bool RecordingReader::Next(RecordedFrame& frame) {
    if (!m_error.empty()) return false;
    if (m_format == RecordingFormat::Trace && m_index == m_traceCount) {
        if (m_input.peek() != std::char_traits<char>::eof())
            m_error = "Trailing data after HXTR records";
        return false;
    }
    if (!m_input.read(reinterpret_cast<char*>(m_buffer.data()), m_buffer.size())) {
        if (!m_input.eof() || m_input.gcount() ||
            (m_format == RecordingFormat::Trace && m_index < m_traceCount))
            m_error = "Truncated or unreadable recording";
        return false;
    }
    ++m_index;
    frame = {};
    if (m_format == RecordingFormat::Hxst) {
        hx_raw_frame packet;
        std::memcpy(&packet, m_buffer.data(), sizeof(packet));
        if (packet.size != sizeof(packet) || packet.version != HX_RAW_ABI_VERSION ||
            packet.payload_size != kNativeTransportSize || packet.reserved[0] ||
            packet.reserved[1] || packet.reserved_tail ||
            packet.flags & ~(HX_RAW_FRAME_SOS | HX_RAW_FRAME_CHARGER)) {
            m_error = "Invalid HXST record";
            return false;
        }
        std::copy(std::begin(packet.payload), std::end(packet.payload), frame.payload.begin());
        frame.length = sizeof(packet.payload);
        frame.timeNs = packet.timestamp_ns;
        frame.epoch = packet.epoch;
        frame.sos = packet.flags & HX_RAW_FRAME_SOS;
        frame.charger = packet.flags & HX_RAW_FRAME_CHARGER;
    } else if (m_format == RecordingFormat::Trace) {
        std::copy(m_buffer.end() - kNativeTransportSize, m_buffer.end(), frame.payload.begin());
        frame.length = kNativeTransportSize;
        frame.timeNs = Le(m_buffer.data(), 8);
        frame.epoch = Le(m_buffer.data() + 12, 4);
        frame.readOk = Le(m_buffer.data() + 16, 4) & 1;
    } else {
        std::copy(m_buffer.begin(), m_buffer.end(), frame.payload.begin());
        frame.length = m_buffer.size();
        frame.timeNs = m_index * 1000000000 / 120;
    }
    return true;
}

}  // namespace himax
