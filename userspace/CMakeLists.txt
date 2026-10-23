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

add_executable(himax-touchd touchd.cpp live-capture.cpp v4l2-capture.cpp input.cpp screen-rotation.cpp control.cpp)
target_include_directories(himax-touchd PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms")
target_link_libraries(himax-touchd PRIVATE himax-tsacore PkgConfig::SYSTEMD)
target_compile_options(himax-touchd PRIVATE -Wall -Wextra -Werror)
install(TARGETS himax-touchd himax-replay RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS himaxctl.py DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME himaxctl)
configure_file(himax-touchd.service.in himax-touchd.service @ONLY)
configure_file(himax-touchd-live-capture.conf.in himax-touchd-live-capture.conf @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/himax-touchd-live-capture.conf"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/himax-touchd")
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
    include(cmake/FrameCheckReference.cmake)
    himax_prepare_frame_check("${UPSTREAM}/EGoTouchService/Device/himax"
        "${TSACORE_OVERLAY}/frame-check-reference.cpp")
    add_executable(himax-frame-check-test tests/frame-check-test.cpp
        "${TSACORE_OVERLAY}/frame-check-reference.cpp")
    target_link_libraries(himax-frame-check-test PRIVATE himax-tsacore)
    target_compile_options(himax-frame-check-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME frame-check-reference COMMAND himax-frame-check-test)
    add_test(NAME frame-check-reference-integration COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/frame-check-reference-test.py" "${CMAKE_COMMAND}"
        "${UPSTREAM}/EGoTouchService/Device/himax"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FrameCheckReference.cmake")
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
    add_executable(himax-capture-buffer-test tests/capture-buffer-test.cpp v4l2-capture.cpp)
    target_include_directories(himax-capture-buffer-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms")
    target_compile_options(himax-capture-buffer-test PRIVATE -Wall -Wextra -Werror)
    target_link_options(himax-capture-buffer-test PRIVATE
        -Wl,--wrap=open,--wrap=close,--wrap=ioctl,--wrap=mmap,--wrap=munmap)
    add_test(NAME capture-two-buffer COMMAND himax-capture-buffer-test)
    include(cmake/RearmReference.cmake)
    himax_prepare_rearm_policy("${UPSTREAM}/EGoTouchService/Device/himax"
        "${TSACORE_OVERLAY}/rearm-policy-windows.cpp")
    himax_prepare_read_mode("${UPSTREAM}/EGoTouchService/Device/himax"
        "${TSACORE_OVERLAY}/rearm-read-mode-windows.cpp")
    add_executable(himax-producer-recovery-test tests/producer-recovery-test.cpp
        "${TSACORE_OVERLAY}/rearm-policy-windows.cpp"
        "${TSACORE_OVERLAY}/rearm-read-mode-windows.cpp")
    target_include_directories(himax-producer-recovery-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/tests"
        "${UPSTREAM}/Common/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms")
    target_compile_options(himax-producer-recovery-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME producer-recovery COMMAND himax-producer-recovery-test)
    add_executable(himax-afe-restart-test tests/afe-restart-test.cpp)
    target_include_directories(himax-afe-restart-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms")
    target_compile_options(himax-afe-restart-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME afe-restart-policy COMMAND himax-afe-restart-test)
    include(cmake/StartupDrainReference.cmake)
    himax_prepare_startup_drain("${UPSTREAM}/EGoTouchService/Device/himax"
        "${TSACORE_OVERLAY}/startup-drain-windows.cpp")
    add_executable(himax-startup-drain-test tests/startup-drain-test.cpp
        "${TSACORE_OVERLAY}/startup-drain-windows.cpp"
        "${TSACORE_OVERLAY}/afe-start-native.c")
    target_include_directories(himax-startup-drain-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tests" "${UPSTREAM}/Common/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms")
    target_compile_options(himax-startup-drain-test PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
    add_test(NAME windows-startup-drain COMMAND himax-startup-drain-test)
    add_test(NAME startup-drain-reference-integration COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/startup-drain-reference-test.py" "${CMAKE_COMMAND}"
        "${UPSTREAM}/EGoTouchService/Device/himax"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StartupDrainReference.cmake")
    himax_prepare_rearm("${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms/himax-v4l2.c"
        "${TSACORE_OVERLAY}/rearm-native.c")
    add_executable(himax-rearm-test tests/rearm-test.cpp "${TSACORE_OVERLAY}/rearm-native.c")
    target_include_directories(himax-rearm-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests"
        "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms")
    target_compile_options(himax-rearm-test PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
    target_link_libraries(himax-rearm-test PRIVATE Threads::Threads)
    add_test(NAME native-producer-rearm COMMAND himax-rearm-test)
    set_tests_properties(native-producer-rearm PROPERTIES TIMEOUT 10)
    add_executable(himax-restart-test tests/restart-test.cpp "${TSACORE_OVERLAY}/rearm-native.c")
    target_include_directories(himax-restart-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests"
        "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms")
    target_compile_options(himax-restart-test PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
    target_link_libraries(himax-restart-test PRIVATE Threads::Threads)
    add_test(NAME native-stalled-restart COMMAND himax-restart-test)
    set_tests_properties(native-stalled-restart PROPERTIES TIMEOUT 10)
    add_test(NAME rearm-reference-integration COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/rearm-reference-test.py" "${CMAKE_COMMAND}"
        "${UPSTREAM}/EGoTouchService/Device/himax"
        "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms/himax-v4l2.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RearmReference.cmake")
    add_executable(himax-recording-test tests/recording-test.cpp)
    target_link_libraries(himax-recording-test PRIVATE himax-tsacore)
    target_compile_options(himax-recording-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME recording-validation COMMAND himax-recording-test $<TARGET_FILE:himax-replay>)
    add_executable(himax-session-test tests/session-test.cpp)
    target_link_libraries(himax-session-test PRIVATE himax-tsacore)
    target_compile_options(himax-session-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME normal-session-recording COMMAND himax-session-test $<TARGET_FILE:himax-replay>)
    add_executable(himax-session-restart-test tests/session-restart-test.cpp)
    target_link_libraries(himax-session-restart-test PRIVATE himax-tsacore)
    target_compile_options(himax-session-restart-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME restart-session-recording COMMAND himax-session-restart-test $<TARGET_FILE:himax-replay>)
    set_tests_properties(restart-session-recording PROPERTIES TIMEOUT 30)
    add_library(himax-touchd-test-main OBJECT touchd.cpp)
    target_link_libraries(himax-touchd-test-main PRIVATE himax-tsacore)
    target_compile_definitions(himax-touchd-test-main PRIVATE main=himax_touchd_main)
    target_compile_options(himax-touchd-test-main PRIVATE -Wall -Wextra -Werror)
    add_executable(himax-daemon-restart-test tests/daemon-restart-test.cpp live-capture.cpp control.cpp
        touch-trace.cpp $<TARGET_OBJECTS:himax-touchd-test-main>)
    target_link_libraries(himax-daemon-restart-test PRIVATE himax-tsacore)
    target_include_directories(himax-daemon-restart-test PRIVATE "${TSACORE}/src")
    target_compile_options(himax-daemon-restart-test PRIVATE -Wall -Wextra -Werror)
    target_link_options(himax-daemon-restart-test PRIVATE -Wl,--wrap=clock_gettime,--wrap=poll)
    add_test(NAME daemon-restart-integration COMMAND himax-daemon-restart-test)
    set_tests_properties(daemon-restart-integration PROPERTIES TIMEOUT 45)
    add_executable(himax-live-capture-test tests/live-capture-test.cpp live-capture.cpp touch-trace.cpp)
    target_include_directories(himax-live-capture-test PRIVATE "${TSACORE}/src")
    target_link_libraries(himax-live-capture-test PRIVATE himax-tsacore)
    target_compile_options(himax-live-capture-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME live-capture COMMAND himax-live-capture-test)
    add_test(NAME live-capture-cli COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/live-capture-cli-test.py" $<TARGET_FILE:himax-touchd>
        "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/capture-v4l2-live.sh")
    set_tests_properties(normal-session-recording live-capture live-capture-cli PROPERTIES TIMEOUT 30)
    add_executable(himax-otp-test ../tests/hx-otp-test.c
        ../touchscreen-hx83121a-dkms/hx-otp.c)
    target_include_directories(himax-otp-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms")
    target_compile_options(himax-otp-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME otp-protocol COMMAND himax-otp-test)
    include(cmake/AhbReference.cmake)
    himax_prepare_ahb_reference("${UPSTREAM}/EGoTouchService/Device/himax"
        "${TSACORE_OVERLAY}/ahb-reference.cpp")
    add_executable(himax-ahb-test ../tests/hx-ahb-test.c
        ../touchscreen-hx83121a-dkms/hx-ahb.c
        "${TSACORE_OVERLAY}/ahb-reference.cpp")
    target_include_directories(himax-ahb-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms")
    target_compile_definitions(himax-ahb-test PRIVATE HX_AHB_UPSTREAM_REFERENCE=1)
    target_compile_options(himax-ahb-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME ahb-protocol COMMAND himax-ahb-test)
    add_test(NAME ahb-reference-integration COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ahb-reference-test.py" "${CMAKE_COMMAND}"
        "${UPSTREAM}/EGoTouchService/Device/himax"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AhbReference.cmake")
    include(cmake/SpiReference.cmake)
    himax_prepare_spi_reference("${UPSTREAM}/EGoTouchService/Device/himax"
        "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms/himax-spi-hw.c"
        "${TSACORE_OVERLAY}")
    add_executable(himax-spi-test tests/spi-test.cpp
        "${TSACORE_OVERLAY}/spi-native.c" "${TSACORE_OVERLAY}/spi-reference.cpp")
    # Upstream WriteBus's WriteFile lambda leaves pBytes unused. Keep its body
    # exact without weakening warnings for native code or the test harness.
    set_source_files_properties("${TSACORE_OVERLAY}/spi-reference.cpp"
        PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter")
    target_include_directories(himax-spi-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_compile_definitions(himax-spi-test PRIVATE CONFIG_TOUCHSCREEN_HIMAX_HX83121A_V4L2=1)
    target_compile_options(himax-spi-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME native-spi-protocol COMMAND himax-spi-test)
    include(cmake/EventReadReference.cmake)
    himax_prepare_event_read("${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms"
        "${TSACORE_OVERLAY}/event-read-native.c")
    add_executable(himax-event-read-test tests/event-read-test.cpp
        "${TSACORE_OVERLAY}/spi-native.c" "${TSACORE_OVERLAY}/event-read-native.c")
    target_include_directories(himax-event-read-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_compile_definitions(himax-event-read-test PRIVATE CONFIG_TOUCHSCREEN_HIMAX_HX83121A_V4L2=1)
    target_compile_options(himax-event-read-test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(himax-event-read-test PRIVATE himax-tsacore)
    add_test(NAME native-event-read COMMAND himax-event-read-test)
    add_test(NAME spi-reference-integration COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/spi-reference-test.py" "${CMAKE_COMMAND}"
        "${UPSTREAM}/EGoTouchService/Device/himax"
        "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms/himax-spi-hw.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SpiReference.cmake")
    add_test(NAME geni-completion-policy COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/geni-completion-test.py" "${CMAKE_C_COMPILER}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/geni-completion-test.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/geni-gsi-completion.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/../kernel-patches/0001-spi-geni-qcom-preserve-in-progress-on-gsi-success.patch")
    add_test(NAME gpi-channel-lifetime COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/gpi-lifetime-test.py" "${CMAKE_C_COMPILER}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/gpi-lifetime-test.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gpi-lifetime.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/../kernel-patches/0002-dmaengine-qcom-gpi-clear-freed-peripheral-config.patch")
    add_test(NAME deployment-kernel-health COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/kernel-health-test.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/lib/kernel-health.sh")
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
    include(cmake/AfeDispatchReference.cmake)
    himax_prepare_afe_dispatch("${UPSTREAM}/EGoTouchService/Device/himax"
        "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms/himax-spi-hw.c"
        "${TSACORE_OVERLAY}")
    add_executable(himax-afe-dispatch-test tests/afe-dispatch-test.cpp
        ../touchscreen-hx83121a-dkms/hx-afe.c
        "${TSACORE_OVERLAY}/afe-dispatch-windows.cpp"
        "${TSACORE_OVERLAY}/afe-dispatch-native.c")
    target_include_directories(himax-afe-dispatch-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(himax-afe-dispatch-test PRIVATE himax-tsacore)
    target_compile_options(himax-afe-dispatch-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME afe-dispatch-reference COMMAND himax-afe-dispatch-test)
    add_test(NAME afe-dispatch-reference-integration COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/afe-dispatch-reference-test.py" "${CMAKE_COMMAND}"
        "${UPSTREAM}/EGoTouchService/Device/himax"
        "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms/himax-spi-hw.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AfeDispatchReference.cmake")
    add_test(NAME afe-reference-integration COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/afe-reference-test.py" "${CMAKE_COMMAND}"
        "${UPSTREAM}/EGoTouchService/Device/himax/HimaxProtocol.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AfeReference.cmake")
    add_executable(himax-raw-policy-test ../tests/hx-raw-policy-test.c)
    target_include_directories(himax-raw-policy-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms")
    target_compile_options(himax-raw-policy-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME raw-transport-policy COMMAND himax-raw-policy-test)
    include(cmake/AfeStartReference.cmake)
    himax_prepare_afe_start("${UPSTREAM}/EGoTouchService/Device/runtime"
        "${UPSTREAM}/EGoTouchService/Device/himax"
        "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms/himax-v4l2.c"
        "${TSACORE_OVERLAY}")
    add_executable(himax-afe-start-test tests/afe-start-test.cpp
        "${TSACORE_OVERLAY}/afe-start-native.c" "${TSACORE_OVERLAY}/afe-start-windows.cpp")
    target_include_directories(himax-afe-start-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests"
        "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms")
    target_compile_options(himax-afe-start-test PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
    add_test(NAME afe-start-retry-reference COMMAND himax-afe-start-test)
    add_executable(himax-afe-start-inject-test tests/afe-start-inject-test.cpp
        "${TSACORE_OVERLAY}/afe-start-native.c")
    target_include_directories(himax-afe-start-inject-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests"
        "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms")
    target_compile_definitions(himax-afe-start-inject-test PRIVATE
        CONFIG_TOUCHSCREEN_HIMAX_HX83121A_AFE_DIAGNOSTICS=1)
    target_compile_options(himax-afe-start-inject-test PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
    add_test(NAME afe-start-injection COMMAND himax-afe-start-inject-test)
    add_test(NAME afe-start-reference-integration COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/afe-start-reference-test.py" "${CMAKE_COMMAND}"
        "${UPSTREAM}/EGoTouchService/Device/runtime"
        "${UPSTREAM}/EGoTouchService/Device/himax"
        "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms/himax-v4l2.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AfeStartReference.cmake")
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
    add_executable(himax-raw-start-test ../tests/hx-raw-start-test.c
        ../touchscreen-hx83121a-dkms/hx-raw-init.c
        "${TSACORE_OVERLAY}/raw-prepare-reference.cpp")
    target_include_directories(himax-raw-start-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms")
    target_compile_definitions(himax-raw-start-test PRIVATE HX_RAW_INIT_UPSTREAM_REFERENCE=1)
    target_compile_options(himax-raw-start-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME raw-master-startup COMMAND himax-raw-start-test)
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
    # Opt-in real startup/reset trial; never registered with CTest.
    add_executable(himax-startup-hardware-test tests/startup-hardware-test.cpp v4l2-capture.cpp)
    target_link_libraries(himax-startup-hardware-test PRIVATE himax-tsacore)
    target_compile_options(himax-startup-hardware-test PRIVATE -Wall -Wextra -Werror)
    add_executable(himax-queue-hardware-test tests/queue-hardware-test.cpp v4l2-capture.cpp)
    target_link_libraries(himax-queue-hardware-test PRIVATE himax-tsacore)
    target_compile_options(himax-queue-hardware-test PRIVATE -Wall -Wextra -Werror)
    # Opt-in hands-off IDLE trial; real SPI and one rearm, never part of CTest.
    add_executable(himax-rearm-hardware-test tests/rearm-hardware-test.cpp v4l2-capture.cpp)
    target_link_libraries(himax-rearm-hardware-test PRIVATE himax-tsacore)
    target_compile_options(himax-rearm-hardware-test PRIVATE -Wall -Wextra -Werror)
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
