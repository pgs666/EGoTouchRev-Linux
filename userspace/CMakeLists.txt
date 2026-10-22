cmake_minimum_required(VERSION 3.20)
project(himax_tsacore VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(CTest)
include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
find_package(Threads REQUIRED)

set(UPSTREAM "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/EGoTouchRev")
set(SOLVERS "${UPSTREAM}/EGoTouchService/Solvers")
set(TSACORE "${SOLVERS}/TouchSolver/TsaCore")
if(NOT EXISTS "${TSACORE}/TsaCore.h")
    message(FATAL_ERROR "Run git submodule update --init third_party/EGoTouchRev")
endif()

option(HIMAX_SANITIZE "Enable address and undefined behaviour sanitizers" OFF)
option(HIMAX_PROFILE "Enable upstream algorithm step timing" OFF)
if(HIMAX_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Compile every active upstream module, without the Windows service, IPC or UI.
# Keep the upstream arithmetic contract; in particular, do not fuse FP operations.
file(GLOB_RECURSE TSACORE_SOURCES CONFIGURE_DEPENDS "${TSACORE}/src/*.cpp")
include(cmake/PrepareUpstream.cmake)
set(TSACORE_OVERLAY "${CMAKE_CURRENT_BINARY_DIR}/upstream-overlay")
himax_prepare_upstream("${TSACORE}" "${TSACORE_OVERLAY}")
list(REMOVE_ITEM TSACORE_SOURCES "${TSACORE}/src/App/Sys.cpp"
    "${TSACORE}/src/App/Tsa.cpp"
    "${TSACORE}/src/Rejection/TouchReport/TouchReport.cpp"
    "${TSACORE}/src/Alg/TouchZone/Contour.cpp")
add_library(himax-tsacore STATIC ${TSACORE_SOURCES}
    "${TSACORE}/HimaxAfeAdapter.cpp" "${TSACORE_OVERLAY}/TsaCoreSolver.cpp"
    "${TSACORE_OVERLAY}/src/App/Sys.cpp" host-screen.cpp edge-onset.cpp
    "${TSACORE_OVERLAY}/src/App/Tsa.cpp"
    "${TSACORE_OVERLAY}/src/Rejection/TouchReport/TouchReport.cpp"
    "${TSACORE_OVERLAY}/src/Alg/TouchZone/Contour.cpp"
    native-frame.cpp recording.cpp capture-session.cpp session-replay.cpp)
target_include_directories(himax-tsacore
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${TSACORE}" "${SOLVERS}"
        "${UPSTREAM}/Common/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms"
    PRIVATE "${TSACORE}/src")
target_compile_options(himax-tsacore PRIVATE
    -fno-strict-aliasing -fwrapv -ffp-contract=off -ftrivial-auto-var-init=zero)
target_compile_definitions(himax-tsacore PUBLIC EGOTOUCH_DIAG=0)
target_link_libraries(himax-tsacore PUBLIC Threads::Threads)
if(HIMAX_PROFILE)
    target_compile_definitions(himax-tsacore PRIVATE TSACORE_PROFILE)
endif()

add_executable(himax-replay replay.cpp touch-trace.cpp)
target_include_directories(himax-replay PRIVATE "${TSACORE}/src")
target_link_libraries(himax-replay PRIVATE himax-tsacore)
target_compile_options(himax-replay PRIVATE -Wall -Wextra -Werror)

add_executable(himax-touchd touchd.cpp v4l2-capture.cpp input.cpp screen-rotation.cpp)
target_include_directories(himax-touchd PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms")
target_link_libraries(himax-touchd PRIVATE himax-tsacore PkgConfig::SYSTEMD)
target_compile_options(himax-touchd PRIVATE -Wall -Wextra -Werror)
install(TARGETS himax-touchd himax-replay RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
configure_file(himax-touchd.service.in himax-touchd.service @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/himax-touchd.service"
    DESTINATION lib/systemd/system)
install(FILES 70-himax-raw.rules DESTINATION lib/udev/rules.d)
install(FILES himax-touchd.modules.conf DESTINATION lib/modules-load.d
    RENAME himax-touchd.conf)
configure_file(himax-screen-bridge.service.in himax-screen-bridge.service @ONLY)
install(PROGRAMS himax-screen-bridge.py DESTINATION ${CMAKE_INSTALL_BINDIR}
    RENAME himax-screen-bridge)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/himax-screen-bridge.service"
    DESTINATION lib/systemd/user)

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(himax-native-test tests/native-test.cpp)
    target_link_libraries(himax-native-test PRIVATE himax-tsacore)
    target_compile_options(himax-native-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME native-frame COMMAND himax-native-test)
    add_test(NAME panel-boe-1300 COMMAND himax-native-test W273AS1300 W273AS1300)
    add_test(NAME panel-boe-1310 COMMAND himax-native-test W273AS1310 W273AS1310)
    add_test(NAME panel-csot-2710 COMMAND himax-native-test W273AS2710 DFLT000000)
    add_executable(himax-input-test tests/input-test.cpp input.cpp)
    target_include_directories(himax-input-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms")
    target_compile_options(himax-input-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME type-b-input COMMAND himax-input-test)
    add_executable(himax-stream-test tests/stream-test.cpp)
    target_include_directories(himax-stream-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms")
    target_compile_options(himax-stream-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME stream-lifecycle COMMAND himax-stream-test)
    add_executable(himax-recording-test tests/recording-test.cpp)
    target_link_libraries(himax-recording-test PRIVATE himax-tsacore)
    target_compile_options(himax-recording-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME recording-validation COMMAND himax-recording-test $<TARGET_FILE:himax-replay>)
    add_executable(himax-session-test tests/session-test.cpp)
    target_link_libraries(himax-session-test PRIVATE himax-tsacore)
    target_compile_options(himax-session-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME normal-session-recording COMMAND himax-session-test $<TARGET_FILE:himax-replay>)
    set_tests_properties(normal-session-recording PROPERTIES TIMEOUT 30)
    add_executable(himax-otp-test ../tests/hx-otp-test.c
        ../touchscreen-hx83121a-dkms/hx-otp.c)
    target_include_directories(himax-otp-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms")
    target_compile_options(himax-otp-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME otp-protocol COMMAND himax-otp-test)
    include(cmake/AfeReference.cmake)
    himax_prepare_afe_reference(
        "${UPSTREAM}/EGoTouchService/Device/himax/HimaxProtocol.cpp"
        "${TSACORE_OVERLAY}/afe-reference.cpp")
    add_executable(himax-afe-test ../tests/hx-afe-test.c
        ../touchscreen-hx83121a-dkms/hx-afe.c "${TSACORE_OVERLAY}/afe-reference.cpp")
    target_include_directories(himax-afe-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms")
    target_compile_definitions(himax-afe-test PRIVATE HX_AFE_UPSTREAM_REFERENCE=1)
    target_compile_options(himax-afe-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME afe-protocol COMMAND himax-afe-test)
    add_test(NAME afe-reference-integration COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/afe-reference-test.py" "${CMAKE_COMMAND}"
        "${UPSTREAM}/EGoTouchService/Device/himax/HimaxProtocol.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AfeReference.cmake")
    add_executable(himax-raw-policy-test ../tests/hx-raw-policy-test.c)
    target_include_directories(himax-raw-policy-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms")
    target_compile_options(himax-raw-policy-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME raw-transport-policy COMMAND himax-raw-policy-test)
    add_executable(himax-raw-init-test ../tests/hx-raw-init-test.c
        ../touchscreen-hx83121a-dkms/hx-raw-init.c)
    target_include_directories(himax-raw-init-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms")
    target_compile_options(himax-raw-init-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME raw-power-on COMMAND himax-raw-init-test)
    include(cmake/RawPrepareReference.cmake)
    himax_prepare_raw_reference("${UPSTREAM}/EGoTouchService/Device/himax"
        "${TSACORE_OVERLAY}/raw-prepare-reference.cpp")
    add_executable(himax-raw-prepare-test ../tests/hx-raw-prepare-test.c
        ../touchscreen-hx83121a-dkms/hx-raw-init.c
        "${TSACORE_OVERLAY}/raw-prepare-reference.cpp")
    target_include_directories(himax-raw-prepare-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms")
    target_compile_definitions(himax-raw-prepare-test PRIVATE HX_RAW_INIT_UPSTREAM_REFERENCE=1)
    target_compile_options(himax-raw-prepare-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME raw-scan-preparation COMMAND himax-raw-prepare-test)
    add_test(NAME raw-preparation-reference-integration COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/raw-prepare-reference-test.py"
        "${CMAKE_COMMAND}" "${UPSTREAM}/EGoTouchService/Device/himax"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RawPrepareReference.cmake")
    # Requires an idle real device and performs hardware resets. Invoke using
    # scripts/test-v4l2-transport.sh; never run as a normal host test.
    add_executable(himax-transport-test tests/transport-test.cpp v4l2-capture.cpp)
    target_include_directories(himax-transport-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms")
    target_compile_options(himax-transport-test PRIVATE -Wall -Wextra -Werror)
    # Also opt-in: sends real calibration/frequency/IDLE commands, records raw
    # suffix transitions and restores the daemon via scripts/test-v4l2-afe.sh.
    add_executable(himax-afe-hardware-test tests/afe-hardware-test.cpp v4l2-capture.cpp)
    target_link_libraries(himax-afe-hardware-test PRIVATE himax-tsacore)
    target_compile_options(himax-afe-hardware-test PRIVATE -Wall -Wextra -Werror)
    add_executable(himax-rotation-test tests/rotation-test.cpp screen-rotation.cpp)
    target_include_directories(himax-rotation-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_options(himax-rotation-test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(himax-rotation-test PRIVATE PkgConfig::SYSTEMD)
    add_test(NAME screen-rotation COMMAND himax-rotation-test)
    add_executable(himax-rotation-socket-test tests/rotation-socket-test.cpp screen-rotation.cpp)
    target_include_directories(himax-rotation-socket-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_options(himax-rotation-socket-test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(himax-rotation-socket-test PRIVATE PkgConfig::SYSTEMD)
    add_test(NAME screen-rotation-socket COMMAND himax-rotation-socket-test)
    add_test(NAME desktop-screen-bridge COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/screen-bridge-test.py")
    add_test(NAME upstream-host-integration COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/upstream-overlay-test.py"
        "${CMAKE_COMMAND}" "${TSACORE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PrepareUpstream.cmake")
    add_executable(himax-solver-screen-test tests/solver-screen-test.cpp screen-rotation.cpp)
    target_include_directories(himax-solver-screen-test PRIVATE "${TSACORE}/src")
    target_link_libraries(himax-solver-screen-test PRIVATE himax-tsacore PkgConfig::SYSTEMD)
    target_compile_options(himax-solver-screen-test PRIVATE -Wall -Wextra -Werror)
    foreach(rotation RANGE 0 3)
        add_test(NAME solver-screen-${rotation} COMMAND himax-solver-screen-test ${rotation})
    endforeach()
    add_executable(himax-solver-edge-test tests/solver-edge-test.cpp)
    target_include_directories(himax-solver-edge-test PRIVATE "${TSACORE}/src")
    target_link_libraries(himax-solver-edge-test PRIVATE himax-tsacore)
    target_compile_options(himax-solver-edge-test PRIVATE -Wall -Wextra -Werror)
    foreach(rotation RANGE 0 3)
        add_test(NAME solver-edge-${rotation} COMMAND himax-solver-edge-test ${rotation})
    endforeach()
    add_executable(himax-solver-edge-reference tests/solver-edge-test.cpp
        tests/edge-onset-reference.cpp)
    target_include_directories(himax-solver-edge-reference PRIVATE "${TSACORE}/src")
    target_link_libraries(himax-solver-edge-reference PRIVATE himax-tsacore)
    target_compile_options(himax-solver-edge-reference PRIVATE -Wall -Wextra -Werror)
    add_test(NAME edge-onset-reference COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/edge-onset-compare.py"
        $<TARGET_FILE:himax-solver-edge-test> $<TARGET_FILE:himax-solver-edge-reference>)
    set_tests_properties(edge-onset-reference PROPERTIES TIMEOUT 120)
    add_executable(himax-edge-evidence-test tests/edge-onset-evidence-test.cpp)
    target_include_directories(himax-edge-evidence-test PRIVATE "${TSACORE}/src")
    target_link_libraries(himax-edge-evidence-test PRIVATE himax-tsacore)
    target_compile_options(himax-edge-evidence-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME edge-onset-evidence COMMAND himax-edge-evidence-test)
endif()
