# Compile the real native SPI functions and exact pinned Windows bus methods.
# The test boundary is spi_sync / PerformSyncIo, not physical SPI / Windows IOCTL.
include("${CMAKE_CURRENT_LIST_DIR}/RawPrepareReference.cmake")

function(himax_prepare_spi_reference directory native output_directory)
    himax_raw_reference_source("${directory}" HimaxProtocol.cpp
        2c4ea2ac280cf0cc81302e0a3d12b01ec72def0d70c5079d345aeae8fdcdf866 protocol)
    himax_raw_reference_source("${directory}" HimaxProtocol.h
        1b0ce9c40778625bfed117941b44c9081a8efb71a39494540e4112a5d78a0ce4 header)
    himax_raw_reference_extract("${protocol}"
        "    ChipResult<> HalDevice::IoctlOnce("
        "    /**\n     * @brief 从设备读取原始数据" ioctl)
    himax_raw_reference_extract("${protocol}"
        "    ChipResult<> HalDevice::ReadBus("
        "    /**\n     * @brief 读取 ACPI 配置数据" bus)
    himax_raw_reference_extract("${protocol}" "constexpr uint8_t OP_WRITE_MASTER"
        "const DWORD SPI_IOCTL_INT_OPEN" operations)
    string(REGEX MATCH "const DWORD SPI_IOCTL_FULL_DUPLEX += 0x[0-9a-fA-F]+" full_duplex "${protocol}")
    if(full_duplex STREQUAL "")
        message(FATAL_ERROR "Missing upstream SPI IOCTL constant")
    endif()
    if(NOT header MATCHES "data_offset = header_size \\+ dummy_size" OR
       NOT header MATCHES "dummy_size = 1" OR NOT header MATCHES "header_size = 2")
        message(FATAL_ERROR "Upstream SPI data offset needs review")
    endif()
    set(windows
"#include \"spi-host.h\"
#include <cstddef>
#include <expected>
#include <vector>
using DWORD = uint32_t;
using LPDWORD = DWORD*;
using LPVOID = void*;
struct OVERLAPPED {};
constexpr DWORD ERROR_OPERATION_ABORTED = 995;
${operations}
${full_duplex};
namespace Himax {
enum class ChipError { InternalError, CommunicationError };
template<class T = void> using ChipResult = std::expected<T, ChipError>;
struct HalDevice {
    hx_spi_test_bus* m_handle;
    void* m_syncEvent = nullptr;
    DWORD m_lastError = 0;
    uint8_t m_readOp = OP_READ_MASTER, m_writeOp = OP_WRITE_MASTER;
    const size_t data_offset = 3;
    std::vector<uint8_t> m_xfer_buffer;
    ChipResult<> IoctlOnce(DWORD, const void*, uint32_t, void*, uint32_t, uint32_t*);
    ChipResult<> Ioctl(DWORD, const void*, uint32_t, void*, uint32_t, uint32_t*);
    ChipResult<> ReadBus(uint8_t, uint8_t*, uint32_t);
    ChipResult<> WriteBus(uint8_t, const uint8_t*, const uint8_t*, uint32_t);
};
thread_local int error;
bool DeviceIoControl(hx_spi_test_bus* bus, DWORD code, void* input, uint32_t inlen,
                     void* output, uint32_t outlen, DWORD* actual, OVERLAPPED*) {
    if (code != SPI_IOCTL_FULL_DUPLEX || inlen != outlen) { error = -EINVAL; return false; }
    error = bus->transfer(bus->context, input, output, inlen, actual);
    return error == 0;
}
bool WriteFile(hx_spi_test_bus* bus, void* input, DWORD len, void*, OVERLAPPED*) {
    uint32_t actual = 0;
    error = bus->transfer(bus->context, input, nullptr, len, &actual);
    return error == 0;
}
// Replace the OS wait/overlapped boundary only. Real Ioctl retry and bus
// length-check methods below are compiled verbatim from the pinned source.
template<class Func>
ChipResult<> PerformSyncIo(hx_spi_test_bus*, void*, DWORD& lastError, Func io,
                          ChipError err, DWORD = 0, bool = false, DWORD* out = nullptr) {
    OVERLAPPED ov;
    DWORD actual = 0;
    if (!io(&ov, &actual)) {
        lastError = error == -ECANCELED ? ERROR_OPERATION_ABORTED : 1;
        return std::unexpected(err);
    }
    lastError = 0;
    if (out) *out = actual;
    return {};
}
${ioctl}
${bus}
}
extern \"C\" int hx_spi_windows_read(hx_spi_test_bus* bus, u8 command, u8* data, u32 len) {
    Himax::HalDevice device;
    device.m_handle = bus;
    return device.ReadBus(command, data, len) ? 0 : -EIO;
}
extern \"C\" int hx_spi_windows_write(hx_spi_test_bus* bus, u8 command,
                                     const u8* address, const u8* data, u32 len) {
    Himax::HalDevice device;
    device.m_handle = bus;
    return device.WriteBus(command, address, data, len) ? 0 : -EIO;
}
")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${native}")
    file(READ "${native}" contents)
    himax_raw_reference_extract("${contents}"
        "#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_V4L2\n#define HIMAX_BUS_RETRY"
        "/* TP SRAM address size" constants)
    foreach(field IN ITEMS HIMAX_REG_SZ HIMAX_SPI_FUNCTION_READ HIMAX_SPI_FUNCTION_WRITE)
        string(REGEX MATCH "#define ${field}[^\n]+" definition "${contents}")
        if(definition STREQUAL "")
            message(FATAL_ERROR "Missing native SPI constant: ${field}")
        endif()
        string(APPEND constants "${definition}\n")
    endforeach()
    himax_raw_reference_extract("${contents}" "int himax_spi_read("
        "#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_V4L2\nstatic int himax_ahb_write(" functions)
    himax_raw_reference_extract("${contents}" "static int himax_ahb_write("
        "static int himax_ahb_read(" adapter)
    set(native_source
"#include \"spi-host.h\"
${constants}
${functions}
${adapter}
int hx_spi_native_write(struct himax_ts_data *ts, u8 command, const u8 *address,
                        const u8 *data, u32 length) {
    return himax_ahb_write(ts, command, address, data, length);
}
")
    file(MAKE_DIRECTORY "${output_directory}")
    foreach(name IN ITEMS windows native_source)
        if(name STREQUAL "windows")
            set(output "${output_directory}/spi-reference.cpp")
        else()
            set(output "${output_directory}/spi-native.c")
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
