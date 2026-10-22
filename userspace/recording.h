// SPDX-License-Identifier: MIT
#pragma once

#include "native-frame.h"

#include <fstream>
#include <string>
#include <vector>

namespace himax {

enum class RecordingFormat { Native, Hxst, Trace };

struct RecordedFrame {
    std::array<uint8_t, kNativeTransportSize> payload{};
    size_t length = 0;
    uint64_t timeNs = 0;
    uint64_t epoch = 0;
    bool sos = false;
    bool charger = false;
    bool readOk = true;
};

class RecordingReader {
public:
    RecordingReader(const char* path, RecordingFormat format, size_t nativeBytes);
    bool Next(RecordedFrame& frame);
    const std::string& Error() const noexcept { return m_error; }

private:
    std::ifstream m_input;
    RecordingFormat m_format;
    std::vector<uint8_t> m_buffer;
    std::string m_error;
    uint64_t m_index = 0;
    uint32_t m_traceCount = 0;
};

}  // namespace himax
