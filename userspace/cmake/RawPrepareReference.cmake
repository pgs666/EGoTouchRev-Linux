# Test-only reference: exact active Windows preparation functions and constants.
# No Windows I/O, kernel code or daemon links this generated implementation.
function(himax_raw_reference_source directory name expected output)
    set(source "${directory}/${name}")
    if(NOT EXISTS "${source}")
        message(FATAL_ERROR "Missing upstream raw preparation source: ${source}")
    endif()
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
    endif()
    file(SHA256 "${source}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Upstream raw preparation reference needs review: ${source}")
    endif()
    file(READ "${source}" contents)
    set("${output}" "${contents}" PARENT_SCOPE)
endfunction()

function(himax_raw_reference_extract contents first last output)
    string(FIND "${contents}" "${first}" start)
    string(FIND "${contents}" "${last}" end)
    if(start LESS 0 OR end LESS start)
        message(FATAL_ERROR "Missing upstream raw preparation delimiters: ${first}")
    endif()
    math(EXPR length "${end} - ${start}")
    string(SUBSTRING "${contents}" ${start} ${length} body)
    set("${output}" "${body}" PARENT_SCOPE)
endfunction()

function(himax_raw_reference_register contents field)
    string(REGEX MATCHALL "op\\.${field} = 0x[0-9A-Fa-f]+" matches "${contents}")
    list(LENGTH matches count)
    if(NOT count EQUAL 1)
        message(FATAL_ERROR "Ambiguous upstream raw preparation register: ${field}")
    endif()
    string(REGEX MATCH "0x[0-9A-Fa-f]+" value "${matches}")
    set("${field}" "${value}" PARENT_SCOPE)
endfunction()

function(himax_prepare_raw_reference directory output)
    himax_raw_reference_source("${directory}" HimaxChipMode.cpp
        f4e65f6f6300799346de1fafc6ec42c1f581706d52eae1aca48a72975648c402 mode)
    himax_raw_reference_source("${directory}" HimaxChipLifecycle.cpp
        d53a9b2f01b1f6d851dad47c36bd9e0c49cfdb961efc81c6859a614520838216 lifecycle)
    himax_raw_reference_source("${directory}" HimaxProtocol.cpp
        2c4ea2ac280cf0cc81302e0a3d12b01ec72def0d70c5079d345aeae8fdcdf866 protocol)
    himax_raw_reference_source("${directory}" HimaxRegisters.cpp
        bb9b1a91cfecd500549e9cc0401a766f06ec5fc7d351b9a0f8dac5dab2de6dfa registers)
    himax_raw_reference_source("${directory}" HimaxByteUtils.h
        7d2c16eb44d3375524d9dc31ce505ec9ab7bc60b515b63d0682be82ce81eceb5 byte_utils)
    himax_raw_reference_extract("${lifecycle}"
        "ChipResult<> Chip::himax_mcu_reload_disable("
        "ChipResult<> Chip::hx_is_reload_done_ahb(" reload)
    himax_raw_reference_extract("${mode}"
        "ChipResult<> Chip::himax_mcu_assign_sorting_mode("
        "ChipResult<> Chip::himax_switch_data_type(" sorting)
    himax_raw_reference_extract("${mode}" "ChipResult<> Chip::hx_set_N_frame("
        "ChipResult<> Chip::himax_switch_mode_inspection(" nframe)
    himax_raw_reference_extract("${mode}" "ChipResult<> Chip::himax_switch_mode_inspection("
        "\n} // namespace Himax" inspection)
    himax_raw_reference_extract("${protocol}"
        "    ChipResult<> HimaxProtocol::write_and_verify("
        "    ChipResult<> HimaxProtocol::safeModeSetRaw(" verify)
    foreach(field IN ITEMS addr_fw_define_flash_reload data_fw_define_flash_reload_dis
            data_fw_define_flash_reload_en addr_rawdata_addr addr_rawdata_end
            addr_sorting_mode_en addr_set_frame_addr)
        himax_raw_reference_register("${registers}" "${field}")
    endforeach()
    # Also guard the invocation order: the reference intentionally covers only
    # this prefix, not physical reset, power-on, raw selection or calibration.
    set(previous -1)
    foreach(call IN ITEMS "himax_mcu_reload_disable(false)" "himax_switch_mode_inspection(THP_INSPECTION_ENUM::EGO_RAWDATA)"
            "hx_set_N_frame(1)" "himax_mcu_power_on_init();")
        string(FIND "${lifecycle}" "if (auto res = ${call}" position)
        if(position LESS 0 OR position LESS_EQUAL previous)
            message(FATAL_ERROR "Upstream raw preparation call order needs review: ${call}")
        endif()
        set(previous "${position}")
    endforeach()
    set(generated
"#include \"hx-raw-init.h\"
#include \"${directory}/HimaxByteUtils.h\"
#include <algorithm>
#include <array>
#include <cerrno>
#include <expected>
#include <memory>
#include <string>
#include <vector>
#define LOG_INFO(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
namespace Himax {
enum class ChipError { CommunicationError, VerificationFailed };
template<class T = void> using ChipResult = std::expected<T, ChipError>;
enum class THP_INSPECTION_ENUM { HX_RAWDATA, HX_ACT_IDLE_RAWDATA, HX_LP_IDLE_RAWDATA, HX_BACK_NORMAL, EGO_RAWDATA };
struct HalDevice {
    const hx_raw_init_bus* bus;
    int error = -EIO;
    bool IsValid() const { return true; }
};
class HimaxProtocol {
public:
    static ChipResult<> register_write(HalDevice* dev, uint32_t address, const uint8_t* data, uint32_t length) {
        int error = dev->bus->write(dev->bus->context, address, data, length);
        if (error) { dev->error = error; return std::unexpected(ChipError::CommunicationError); }
        return {};
    }
    static ChipResult<> register_read(HalDevice* dev, uint32_t address, uint8_t* data, uint32_t length) {
        int error = dev->bus->read(dev->bus->context, address, data, length);
        if (error) { dev->error = error; return std::unexpected(ChipError::CommunicationError); }
        return {};
    }
    static ChipResult<> write_and_verify(HalDevice*, uint32_t, const uint8_t*, uint32_t, uint32_t = 0);
};
class Chip {
public:
    explicit Chip(const hx_raw_init_bus* bus) : m_master(std::make_unique<HalDevice>(bus)) {}
    ChipResult<> himax_mcu_reload_disable(uint8_t);
    ChipResult<> himax_mcu_assign_sorting_mode(uint8_t*);
    ChipResult<> hx_set_N_frame(uint8_t);
    ChipResult<> himax_switch_mode_inspection(THP_INSPECTION_ENUM);
    std::unique_ptr<HalDevice> m_master;
    struct {
        uint32_t addr_fw_define_flash_reload = ${addr_fw_define_flash_reload};
        uint32_t data_fw_define_flash_reload_dis = ${data_fw_define_flash_reload_dis};
        uint32_t data_fw_define_flash_reload_en = ${data_fw_define_flash_reload_en};
    } pdriver_op;
    struct {
        uint32_t addr_sorting_mode_en = ${addr_sorting_mode_en};
        uint32_t addr_set_frame_addr = ${addr_set_frame_addr};
    } pfw_op;
    struct {
        uint32_t addr_rawdata_addr = ${addr_rawdata_addr};
        uint32_t addr_rawdata_end = ${addr_rawdata_end};
    } psram_op;
};
${verify}
${reload}
${sorting}
${nframe}
${inspection}
}
extern \"C\" int hx_raw_prepare_reference(const hx_raw_init_bus* bus) {
    Himax::Chip chip(bus);
    auto result = chip.himax_mcu_reload_disable(false);
    if (result) result = chip.himax_switch_mode_inspection(Himax::THP_INSPECTION_ENUM::EGO_RAWDATA);
    if (result) result = chip.hx_set_N_frame(1);
    if (result) return 0;
    return result.error() == Himax::ChipError::VerificationFailed ? -EINVAL : chip.m_master->error;
}
")
    set(previous "")
    if(EXISTS "${output}")
        file(READ "${output}" previous)
    endif()
    if(NOT generated STREQUAL previous)
        get_filename_component(parent "${output}" DIRECTORY)
        file(MAKE_DIRECTORY "${parent}")
        file(WRITE "${output}" "${generated}")
    endif()
endfunction()
