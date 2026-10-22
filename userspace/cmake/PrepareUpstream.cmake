# Keep the remotely fetchable submodule untouched. Generated sources add the
# host direction hooks, a bounded first-report correction and contour bounds.
# Exact source hashes deliberately require review when upstream changes.
function(himax_overlay_source source output expected_hash anchor header insertion)
    if(NOT EXISTS "${source}")
        message(FATAL_ERROR "Missing upstream source: ${source}")
    endif()
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
    endif()
    file(SHA256 "${source}" actual_hash)
    if(NOT actual_hash STREQUAL expected_hash)
        message(FATAL_ERROR
            "Upstream host integration needs review: ${source}\n"
            "Expected ${expected_hash}, got ${actual_hash}. "
            "Review the integration point before updating PrepareUpstream.cmake.")
    endif()
    file(READ "${source}" contents)
    string(FIND "${contents}" "${anchor}" offset)
    if(offset LESS 0)
        message(FATAL_ERROR "Missing upstream integration point: ${source}")
    endif()
    string(REPLACE "${anchor}"
        "${anchor}\n${insertion}" contents "${contents}")
    set(generated "#line 1 \"${source}\"\n${contents}")
    if(NOT header STREQUAL "")
        set(generated "#include \"${header}\"\n${generated}")
    endif()
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

function(himax_prepare_upstream source_dir output_dir)
    # Apply after the pen session's default direction, so screen state never
    # needs to travel through an otherwise-disabled pen status word.
    himax_overlay_source("${source_dir}/TsaCoreSolver.cpp"
        "${output_dir}/TsaCoreSolver.cpp"
        ad5e80c15ca644d04ef32908eebeff9921fbad7769462f56525a3deb3de95c23
        "    ApplyPenSession();" "host-screen.h" "    ::himax::ApplyHostScreenRotation();")
    # MsReset -> Sys::Init continues into the same frame's touch/grip solver.
    # Restore direction here, before any later initialization/processing reads it.
    himax_overlay_source("${source_dir}/src/App/Sys.cpp"
        "${output_dir}/src/App/Sys.cpp"
        0db1913ca6b77a31c8aa0a114734685a5badd095cf6e362ce0ea6ca0608c56f0
        "    s.screenRotation = 0;" "host-screen.h" "    ::himax::ApplyHostScreenRotation();")
    # Admission/grip tests and history/filtering have already run. Do not set
    # the history-replay flag: upstream lets it bypass ToBeReported and changes
    # lift-off debounce as a side effect. Correct only the first admitted point.
    himax_overlay_source("${source_dir}/src/Rejection/TouchReport/TouchReport.cpp"
        "${output_dir}/src/Rejection/TouchReport/TouchReport.cpp"
        360d290a4a77fb9f62ff949f301305e1eb4978b38b2e1f99491d9338af9cb641
        "    if (replayed) u.pos = orig;" "edge-onset.h"
        "    ::himax::RecoverEdgeOnset(curIdx, prevIdx, report);")
    # At the last physical sensor, first=len-2 needs TWO repeated edge taps.
    # Upstream only repeats tap 3, reading row/column len even if its weight is
    # zero. Preserve every previously valid tap and bound only out-of-grid taps.
    himax_overlay_source("${source_dir}/src/Alg/TouchZone/Contour.cpp"
        "${output_dir}/src/Alg/TouchZone/Contour.cpp"
        aba01d0b6c3f242f7264ba97c00c3fa0358933bc29119ef7e9a595c4fbc06d59
        "int WindowLine(int first, int k, int len) {" ""
        "    if (first + k < 0 || first + k >= len) return std::clamp(first + k, 0, len - 1);")
endfunction()
