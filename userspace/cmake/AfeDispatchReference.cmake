# Test-only: unchanged native dispatcher and pinned Windows AFE methods.
include("${CMAKE_CURRENT_LIST_DIR}/RawPrepareReference.cmake")

function(himax_prepare_afe_dispatch directory native output_directory)
    himax_raw_reference_source("${directory}" HimaxAfe.cpp
        4d0f81896d80db097d4513dd884101172e76fddbb3b5b9c99148271e8472e615 source)
    himax_raw_reference_extract("${source}" "ChipResult<> AfeController::EnterIdle("
        "// ── 手写笔生命周期" methods)
    set(windows "#include \"afe-dispatch-host.h\"
#include <chrono>
#include <expected>
#define LOG_INFO(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
namespace Himax {
enum class ChipError { InvalidOperation, CommunicationError };
enum class ConnectionState { Connected, Unconnected };
template<class T = void> using ChipResult = std::expected<T, ChipError>;
struct Chip {
    hx_dispatch_device* device;
    ConnectionState GetConnectionState() const {
        return device->connected ? ConnectionState::Connected : ConnectionState::Unconnected;
    }
    hx_dispatch_device* GetMasterDevice() { return device; }
    u8& GetCurrentSlot() { return device->slot; }
    void ArmIdleRetry() { device->idle_retry = 1; }
};
namespace HimaxProtocol {
ChipResult<> send_command(hx_dispatch_device* d, u8 command, u8 value, u8& slot) {
    if (hx_afe_send(&d->bus, &slot, command, value))
        return std::unexpected(ChipError::CommunicationError);
    return {};
}
}
class AfeController {
public:
    explicit AfeController(hx_dispatch_device* d) : m_chip{d} {}
    ChipResult<> EnterIdle(u8);
    ChipResult<> ForceExitIdle();
    ChipResult<> StartCalibration(u8);
    ChipResult<> ClearStatus(u8);
    ChipResult<> ForceToFreqPoint(u8);
    ChipResult<> ForceToScanRate(u8);
private:
    Chip m_chip;
    std::chrono::steady_clock::time_point m_lastEnterIdleLog{};
};
${methods}
}
extern \"C\" int hx_dispatch_windows(hx_dispatch_device* device, u32 action, u8 parameter) {
    Himax::AfeController afe(device);
    Himax::ChipResult<> result = std::unexpected(Himax::ChipError::InvalidOperation);
    switch (action) {
    case HX_RAW_CLEAR_STATUS: result = afe.ClearStatus(parameter); break;
    case HX_RAW_FORCE_FREQ: result = afe.ForceToFreqPoint(parameter); break;
    case HX_RAW_FORCE_RATE: result = afe.ForceToScanRate(parameter); break;
    case HX_RAW_ENTER_IDLE: result = afe.EnterIdle(parameter); break;
    case HX_RAW_CALIBRATE: result = afe.StartCalibration(parameter); break;
    case HX_RAW_FORCE_EXIT_IDLE: if (!parameter) result = afe.ForceExitIdle(); break;
    }
    return result ? 0 : -1;
}
")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${native}")
    file(READ "${native}" native_source)
    himax_raw_reference_extract("${native_source}" "int himax_afe_command("
        "#else\nint himax_restore_afe_runtime(" dispatch)
    set(native_body "#include \"afe-dispatch-host.h\"
#include <errno.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
struct himax_ts_data { struct hx_afe_bus bus; u8 afe_command_slot; };
static int himax_afe_send_command(struct himax_ts_data* ts, u8* slot, u8 command, u8 value) {
    return hx_afe_send(&ts->bus, slot, command, value);
}
${dispatch}
int hx_dispatch_native(struct hx_dispatch_device* device, u32 action, u32 parameter) {
    struct himax_ts_data ts = { device->bus, device->slot };
    int result = himax_afe_command(&ts, action, parameter);
    device->slot = ts.afe_command_slot;
    return result;
}
")
    foreach(name IN ITEMS windows native_body)
        set(output "${output_directory}/afe-dispatch-${name}.cpp")
        if(name STREQUAL "native_body")
            set(output "${output_directory}/afe-dispatch-native.c")
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
