# Test-only: compile the exact upstream packet builder without Windows I/O.
# The complete source hash and delimiters require review on an upstream update.
function(himax_prepare_afe_reference source output)
    if(NOT EXISTS "${source}")
        message(FATAL_ERROR "Missing upstream AFE source: ${source}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
    file(SHA256 "${source}" actual)
    if(NOT actual STREQUAL "2c4ea2ac280cf0cc81302e0a3d12b01ec72def0d70c5079d345aeae8fdcdf866")
        message(FATAL_ERROR "Upstream AFE reference needs review: ${source}")
    endif()
    file(READ "${source}" contents)
    string(FIND "${contents}" "    void HimaxProtocol::build_command_packet(" start)
    string(FIND "${contents}" "    ChipResult<> HimaxProtocol::write_and_verify(" end)
    if(start LESS 0 OR end LESS start)
        message(FATAL_ERROR "Missing upstream AFE reference delimiters")
    endif()
    math(EXPR length "${end} - ${start}")
    string(SUBSTRING "${contents}" ${start} ${length} builder)
    set(generated
"#include <array>
#include <cstdint>
#include <cstring>
namespace Himax::HimaxProtocol {
void build_command_packet(uint8_t, uint8_t, uint8_t*);
}
namespace Himax {
#line 433 \"${source}\"
${builder}
}
extern \"C\" void hx_afe_reference_packet(uint8_t command, uint8_t value, uint8_t* packet) {
    Himax::HimaxProtocol::build_command_packet(command, value, packet);
}
")
    set(previous "")
    if(EXISTS "${output}")
        file(READ "${output}" previous)
    endif()
    if(NOT generated STREQUAL previous)
        get_filename_component(directory "${output}" DIRECTORY)
        file(MAKE_DIRECTORY "${directory}")
        file(WRITE "${output}" "${generated}")
    endif()
endfunction()
