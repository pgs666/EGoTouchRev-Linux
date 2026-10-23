# Production one-attempt startup/retry code and pinned Windows delay policy.
include("${CMAKE_CURRENT_LIST_DIR}/RawPrepareReference.cmake")

function(himax_prepare_afe_start runtime chip native output_directory)
    himax_raw_reference_source("${runtime}" DeviceRuntime.cpp
        2b81a9c104324c9982eaece3aa6f48e1a3218e127be1ec3f15659447e143aea3 windows_source)
    himax_raw_reference_extract("${windows_source}"
        "std::chrono::milliseconds BringUpBackoff(" "const char *StandbyReason(" backoff)
    string(REGEX MATCH "constexpr std::chrono::milliseconds kBusPollInterval\\{[0-9]+\\}"
        bus_poll "${windows_source}")
    if(bus_poll STREQUAL "")
        message(FATAL_ERROR "Missing Windows unpowered bus poll interval")
    endif()
    himax_raw_reference_source("${chip}" HimaxChip.cpp
        e0bc0a2bdc2fe575424c1af5aa9f6c717f4e4b488dee96fcc0c4660b0aa3be56 chip_source)
    himax_raw_reference_source("${chip}" HimaxChip.h
        0e089abaf377412068102b1146118ea99a55e83080dfbbed9d5baa057a910896 chip_header)
    himax_raw_reference_extract("${chip_header}" "            struct BusProbe {"
        "            BusProbe ProbeBus();" bus_probe)
    himax_raw_reference_extract("${chip_source}" "Chip::BusProbe Chip::ProbeBus()"
        "void Chip::CancelPendingFrameRead()" bus_check)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${native}")
    file(READ "${native}" source)
    himax_raw_reference_extract("${source}" "static int himax_start_afe("
        "static void himax_return_buffers(" startup)
    himax_raw_reference_extract("${source}" "static int himax_stream_start("
        "static const struct vb2_ops" stream)
    set(native_body "#include \"afe-start-host.h\"
${startup}
${stream}
int hx_start_once(struct himax_ts_data* ts) { return himax_start_afe(ts); }
void hx_start_or_retry(struct himax_ts_data* ts) { himax_start_or_retry(ts); }
int hx_stream_on(struct himax_ts_data* ts) {
    struct vb2_queue q = { ts };
    return himax_stream_start(&q, 2);
}
void hx_stream_off(struct himax_ts_data* ts) {
    struct vb2_queue q = { ts };
    himax_stream_stop(&q);
}
")
    set(windows_body "#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
${backoff}
${bus_poll};
#define LOG_WARN(...) ((void)0)
namespace Himax {
enum class ChipError { CommunicationError, InvalidOperation };
template<class T = void> using ChipResult = std::expected<T, ChipError>;
struct Device {
    uint8_t value;
    bool ioOk;
    bool IsValid() const { return true; }
    unsigned GetError() const { return 1; }
    ChipResult<> ReadBus(unsigned command, uint8_t *output, unsigned length) {
        if (!ioOk || command != 0x13 || length != 1)
            return std::unexpected(ChipError::CommunicationError);
        *output = value;
        return {};
    }
};
struct Chip {
    Device *m_master, *m_slave;
${bus_probe}
    BusProbe ProbeBus();
    ChipResult<> check_bus();
};
${bus_check}
}
extern \"C\" unsigned hx_windows_backoff(int failures) {
    return static_cast<unsigned>(BringUpBackoff(failures).count());
}
extern \"C\" bool hx_windows_bus_ready(bool io_ok, unsigned char master, unsigned char slave) {
    Himax::Device m{master, io_ok}, s{slave, io_ok};
    Himax::Chip chip{&m, &s};
    return chip.check_bus().has_value();
}
extern \"C\" unsigned hx_windows_bus_poll() {
    return static_cast<unsigned>(kBusPollInterval.count());
}
")
    foreach(name IN ITEMS native_body windows_body)
        if(name STREQUAL "native_body")
            set(output "${output_directory}/afe-start-native.c")
        else()
            set(output "${output_directory}/afe-start-windows.cpp")
        endif()
        set(previous "")
        if(EXISTS "${output}")
            file(READ "${output}" previous)
        endif()
        if(NOT previous STREQUAL "${${name}}")
            file(WRITE "${output}" "${${name}}")
        endif()
    endforeach()
endfunction()
