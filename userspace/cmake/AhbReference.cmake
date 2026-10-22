# Exact upstream bus-level functions, linked into tests only.
include("${CMAKE_CURRENT_LIST_DIR}/RawPrepareReference.cmake")

function(himax_prepare_ahb_reference directory output)
    himax_raw_reference_source("${directory}" HimaxProtocol.cpp
        2c4ea2ac280cf0cc81302e0a3d12b01ec72def0d70c5079d345aeae8fdcdf866 protocol)
    himax_raw_reference_source("${directory}" HimaxChipLifecycle.cpp
        d53a9b2f01b1f6d851dad47c36bd9e0c49cfdb961efc81c6859a614520838216 lifecycle)
    himax_raw_reference_source("${directory}" HimaxRegisters.cpp
        bb9b1a91cfecd500549e9cc0401a766f06ec5fc7d351b9a0f8dac5dab2de6dfa registers)
    himax_raw_reference_source("${directory}" HimaxByteUtils.h
        7d2c16eb44d3375524d9dc31ce505ec9ab7bc60b515b63d0682be82ce81eceb5 byte_utils)
    himax_raw_reference_extract("${protocol}"
        "    ChipResult<> HimaxProtocol::burst_enable("
        "    void HimaxProtocol::build_command_packet(" bus_functions)
    himax_raw_reference_extract("${lifecycle}"
        "ChipResult<> Chip::himax_mcu_interface_on("
        "ChipResult<> Chip::hx_sense_on(" interface_on)
    foreach(field IN ITEMS addr_ahb_rdata_byte_0 addr_conti addr_incr4
            data_conti data_incr4)
        himax_raw_reference_register("${registers}" "${field}")
    endforeach()
    set(generated
"#include \"hx-ahb.h\"
#include \"${directory}/HimaxByteUtils.h\"
#include <array>
#include <cerrno>
#include <expected>
#include <memory>
#define LOG_INFO(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
namespace Himax {
enum class ChipError { CommunicationError, InvalidOperation };
template<class T = void> using ChipResult = std::expected<T, ChipError>;
struct HalDevice {
    const hx_ahb_bus* bus;
    int error = -EIO;
    bool IsValid() const { return true; }
    ChipResult<> ReadBus(uint8_t command, uint8_t* data, uint32_t length) {
        int result = bus->read(bus->context, command, data, length);
        if (result) { error = result; return std::unexpected(ChipError::CommunicationError); }
        return {};
    }
    ChipResult<> WriteBus(uint8_t command, const uint8_t* address, const uint8_t* data, uint32_t length) {
        int result = bus->write(bus->context, command, address, data, length);
        if (result) { error = result; return std::unexpected(ChipError::CommunicationError); }
        return {};
    }
};
class HimaxProtocol {
public:
    static ChipResult<> burst_enable(HalDevice*, bool);
    static ChipResult<> register_read(HalDevice*, uint32_t, uint8_t*, uint32_t);
    static ChipResult<> register_write(HalDevice*, uint32_t, const uint8_t*, uint32_t);
};
class Chip {
public:
    explicit Chip(const hx_ahb_bus* bus) : m_master(std::make_unique<HalDevice>(bus)) {}
    ChipResult<> himax_mcu_interface_on();
    void Sleep(unsigned int ms) { m_master->bus->sleep_ms(m_master->bus->context, ms); }
    std::unique_ptr<HalDevice> m_master;
    struct {
        uint8_t addr_ahb_rdata_byte_0 = ${addr_ahb_rdata_byte_0};
        uint8_t addr_conti = ${addr_conti};
        uint8_t addr_incr4 = ${addr_incr4};
        uint8_t data_conti = ${data_conti};
        uint8_t data_incr4 = ${data_incr4};
    } pic_op;
};
${bus_functions}
${interface_on}
}
extern \"C\" int hx_ahb_reference(const hx_ahb_bus* bus, unsigned int operation,
                                 uint32_t address, uint8_t* data, uint32_t length) {
    Himax::Chip chip(bus);
    Himax::ChipResult<> result;
    switch (operation) {
    case 0: result = Himax::HimaxProtocol::burst_enable(chip.m_master.get(), length != 0); break;
    case 1: result = Himax::HimaxProtocol::register_read(chip.m_master.get(), address, data, length); break;
    case 2: result = Himax::HimaxProtocol::register_write(chip.m_master.get(), address, data, length); break;
    case 3: result = chip.himax_mcu_interface_on(); break;
    default: return -EINVAL;
    }
    if (result) return 0;
    return result.error() == Himax::ChipError::InvalidOperation ? -EINVAL : chip.m_master->error;
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
