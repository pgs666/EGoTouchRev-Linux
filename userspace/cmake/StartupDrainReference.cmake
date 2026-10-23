# Test-only: original Windows producer opening/read policy/queue cleanup.
# HalDevice calls are scripted; this does not emulate KMDF, IRQ or physical SPI.
include("${CMAKE_CURRENT_LIST_DIR}/RawPrepareReference.cmake")

function(himax_prepare_startup_drain directory output)
    himax_raw_reference_source("${directory}" HimaxChipLifecycle.cpp
        d53a9b2f01b1f6d851dad47c36bd9e0c49cfdb961efc81c6859a614520838216 lifecycle)
    himax_raw_reference_source("${directory}" HimaxChipFrame.cpp
        ed7e83db454993c726c0595c0775f84e7ad8d07809a4aec5f22360b37c96d54d frames)
    himax_raw_reference_extract("${lifecycle}" "    if (auto res = m_master->IntOpen(); !res)"
        "    m_lastInitFailStep = \"\";" cleanup)
    himax_raw_reference_extract("${frames}" "ChipResult<> Chip::ApplyFrameReadPolicy()"
        "void Chip::ResetFrameReadState()" read_policy)
    string(REGEX MATCH "constexpr uint8_t +kFrameReadTimeoutMs[^\n]+" timeout "${frames}")
    if(timeout STREQUAL "")
        message(FATAL_ERROR "Missing Windows startup read timeout")
    endif()
    set(generated "#include \"startup-drain-host.h\"
#include \"FrameLayout.h\"
#include <array>
#include <expected>
template<class... Args> static void TestLog(const Args&...) {}
#define LOG_INFO(...) TestLog(__VA_ARGS__)
#define LOG_ERROR(...) TestLog(__VA_ARGS__)
${timeout}
namespace Himax {
enum class ChipError { InvalidOperation, CommunicationError };
template<class T = void> using ChipResult = std::expected<T, ChipError>;
struct HalDevice {
    hx_startup_drain_bus* bus;
    unsigned channel;
    int Call(unsigned operation, unsigned value = 0) {
        return bus->operation(bus->context, channel, operation, value);
    }
    ChipResult<> Command(unsigned op, unsigned value = 0) {
        if (Call(op, value)) return std::unexpected(ChipError::CommunicationError);
        return {};
    }
    bool IsValid() { return !Call(HX_DRAIN_VALID); }
    unsigned GetError() { return 1; }
    ChipResult<> IntOpen() { return Command(HX_DRAIN_OPEN); }
    ChipResult<> IntClose() { return Command(HX_DRAIN_CLOSE); }
    ChipResult<> SetBlock(bool block) { return Command(HX_DRAIN_BLOCK, block); }
    ChipResult<> SetTimeOut(uint8_t timeout) { return Command(HX_DRAIN_TIMEOUT, timeout); }
    bool GetFrame(uint8_t*, uint32_t bytes, void*) {
        return !Call(HX_DRAIN_FRAME, bytes);
    }
};
struct Handle {
    HalDevice device;
    HalDevice* get() { return &device; }
    HalDevice* operator->() { return &device; }
};
struct Chip {
    Handle m_master, m_slave;
    std::array<uint8_t, Frame::kTotalFrameSize> back_data{};
    const char* GetStateStr() const { return \"test\"; }
    ChipResult<> InitFailed(const char*, ChipResult<> result) { return result; }
    ChipResult<> ApplyFrameReadPolicy();
    ChipResult<> Cleanup(unsigned& discarded) {
${cleanup}
        discarded = static_cast<unsigned>(staleFrames);
        return {};
    }
};
${read_policy}
}
extern \"C\" int hx_windows_startup_drain(hx_startup_drain_bus* bus, unsigned* discarded) {
    Himax::Chip chip{{{bus, 0}}, {{bus, 1}}};
    return chip.Cleanup(*discarded) ? 0 : -1;
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
