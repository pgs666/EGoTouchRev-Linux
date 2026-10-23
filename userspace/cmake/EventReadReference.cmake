# Compile the production event-read function; mock only its physical SPI I/O.
include("${CMAKE_CURRENT_LIST_DIR}/RawPrepareReference.cmake")

function(himax_prepare_event_read directory output)
    set(source "${directory}/himax-v4l2.c")
    set(header "${directory}/himax-v4l2.h")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}" "${header}")
    file(READ "${source}" contents)
    file(READ "${header}" definitions)
    himax_raw_reference_extract("${contents}" "static int himax_read_raw("
        "#include \"himax-v4l2-startup.h\"" function_body)
    set(constants "")
    foreach(field IN ITEMS HIMAX_FULL_EVENT_STACK_SIZE HIMAX_BUS_READ_HEADER_SIZE
                           HIMAX_EVENT_STACK_ADDRESS)
        string(REGEX MATCH "#define ${field}[^\n]+" definition "${definitions}")
        if(definition STREQUAL "")
            message(FATAL_ERROR "Missing event read constant: ${field}")
        endif()
        string(APPEND constants "${definition}\n")
    endforeach()
    set(generated
"#include \"spi-host.h\"
#include \"hx-v4l2-uapi.h\"
${constants}
${function_body}
int hx_spi_native_read_event(struct himax_ts_data *ts) { return himax_read_raw(ts); }
")
    set(previous "")
    if(EXISTS "${output}")
        file(READ "${output}" previous)
    endif()
    if(NOT previous STREQUAL generated)
        file(WRITE "${output}" "${generated}")
    endif()
endfunction()
