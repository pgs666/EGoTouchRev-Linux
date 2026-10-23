# Compile the actual shared capture, no-reset request validation and IRQ body.
# Only kernel primitives and physical SPI are replaced by test doubles.
include("${CMAKE_CURRENT_LIST_DIR}/RawPrepareReference.cmake")
function(himax_prepare_rearm native output)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${native}")
    file(READ "${native}" source)
    himax_raw_reference_extract("${source}" "struct himax_video_buffer {"
        "static void himax_state_event(" buffer)
    himax_raw_reference_extract("${source}" "static int himax_capture_frame("
        "static int himax_queue_setup(" capture)
    set(generated "#include \"rearm-host.h\"
${buffer}
${capture}
int hx_rearm(struct himax_ts_data *ts, struct file *file, const struct hx_raw_rearm *request) {
    mutex_lock(&ts->op_lock);
    int result = himax_rearm(ts, file, request);
    mutex_unlock(&ts->op_lock);
    return result;
}
int hx_irq(struct himax_ts_data *ts) { return himax_raw_irq(0, ts); }
void hx_enqueue(struct himax_ts_data *ts, void *storage) {
    struct himax_video_buffer *buf = storage;
    buf->link.next = &ts->buffers;
    buf->link.prev = ts->buffers.prev;
    ts->buffers.prev->next = &buf->link;
    ts->buffers.prev = &buf->link;
}
unsigned long hx_buffer_size(void) { return sizeof(struct himax_video_buffer); }
")
    set(previous "")
    if(EXISTS "${output}")
        file(READ "${output}" previous)
    endif()
    if(NOT previous STREQUAL generated)
        file(WRITE "${output}" "${generated}")
    endif()
endfunction()

function(himax_prepare_rearm_policy upstream output)
    himax_raw_reference_source("${upstream}" HimaxChipFrame.cpp
        ed7e83db454993c726c0595c0775f84e7ad8d07809a4aec5f22360b37c96d54d source)
    himax_raw_reference_extract("${source}" "constexpr uint8_t  kFrameReadTimeoutMs"
        "constexpr uint32_t kBadFrameLimit" constants)
    himax_raw_reference_extract("${source}" "    enum class Read :"
        "    // ── slave（笔）──" policy)
    # Replace the OS sleep only, not its argument or the retry predicates.
    string(REPLACE "std::this_thread::sleep_for" "TestSleep" policy "${policy}")
    set(generated "#include <chrono>
#include <cstdint>
#include \"rearm-policy-host.h\"
${constants}
extern \"C\" unsigned hx_windows_rearm_schedule(bool idle, unsigned elapsed,
                                               unsigned *schedule, unsigned capacity) {
    using clock = TestClock;
    TestClock::time = 0;
    const auto start = clock::now();
    TestClock::time = elapsed;
    bool m_idleRetry = idle;
    uint32_t m_readStalls = 0;
    const char *m_sosReason = nullptr;
    HalDevice device{schedule, capacity, 0, kFrameReadTimeoutMs};
${policy}
    (void)readChannel(&device, nullptr, 5063, \"Master\", \"no master frame\");
    (void)m_sosReason;
    return device.count;
}
")
    set(previous "")
    if(EXISTS "${output}")
        file(READ "${output}" previous)
    endif()
    if(NOT previous STREQUAL generated)
        file(WRITE "${output}" "${generated}")
    endif()
endfunction()
