# Keep the remotely fetchable submodule untouched. These two generated sources
# add Linux host-state integration, not a fork of the touch algorithm.
# Exact source hashes deliberately require review when upstream changes.
function(himax_overlay_source source output expected_hash anchor)
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
        "${anchor}\n    ::himax::ApplyHostScreenRotation();" contents "${contents}")
    set(generated "#include \"host-screen.h\"\n#line 1 \"${source}\"\n${contents}")
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
        "    ApplyPenSession();")
    # MsReset -> Sys::Init continues into the same frame's touch/grip solver.
    # Restore direction here, before any later initialization/processing reads it.
    himax_overlay_source("${source_dir}/src/App/Sys.cpp"
        "${output_dir}/src/App/Sys.cpp"
        0db1913ca6b77a31c8aa0a114734685a5badd095cf6e362ce0ea6ca0608c56f0
        "    s.screenRotation = 0;")
endfunction()
