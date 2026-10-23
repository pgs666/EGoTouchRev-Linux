# Production one-attempt startup/retry code and pinned Windows delay policy.
include("${CMAKE_CURRENT_LIST_DIR}/RawPrepareReference.cmake")

function(himax_prepare_afe_start runtime native output_directory)
    himax_raw_reference_source("${runtime}" DeviceRuntime.cpp
        2b81a9c104324c9982eaece3aa6f48e1a3218e127be1ec3f15659447e143aea3 windows_source)
    himax_raw_reference_extract("${windows_source}"
        "std::chrono::milliseconds BringUpBackoff(" "const char *StandbyReason(" backoff)
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
${backoff}
extern \"C\" unsigned hx_windows_backoff(int failures) {
    return static_cast<unsigned>(BringUpBackoff(failures).count());
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
