# Test-only oracle: compile the pinned Windows CheckFrame body unchanged.
include("${CMAKE_CURRENT_LIST_DIR}/RawPrepareReference.cmake")

function(himax_prepare_frame_check directory output)
    himax_raw_reference_source("${directory}" HimaxChipFrame.cpp
        ed7e83db454993c726c0595c0775f84e7ad8d07809a4aec5f22360b37c96d54d source)
    himax_raw_reference_extract("${source}" "enum class FrameCheck"
        "// 无效的 slave" body)
    set(constants "")
    foreach(name IN ITEMS kChecksumOffset kBusFiller)
        string(REGEX MATCH "constexpr uint[0-9]+_t +${name}[^\n]+" declaration "${source}")
        if(declaration STREQUAL "")
            message(FATAL_ERROR "Missing upstream frame-check constant: ${name}")
        endif()
        string(APPEND constants "${declaration}\n")
    endforeach()
    set(generated "#include <cstdint>
#include \"native-frame.h\"
namespace {
${constants}
${body}
}
himax::FrameError WindowsMasterCheck(const uint8_t* data) {
    switch (CheckFrame(data, Frame::kMasterFrameSize)) {
    case FrameCheck::Valid: return himax::FrameError::None;
    case FrameCheck::AllZero: return himax::FrameError::AllZero;
    case FrameCheck::ChecksumError: return himax::FrameError::Checksum;
    case FrameCheck::BusFiller: return himax::FrameError::BusFiller;
    }
    return himax::FrameError::Count;
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
