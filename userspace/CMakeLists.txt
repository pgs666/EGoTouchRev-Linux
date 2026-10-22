cmake_minimum_required(VERSION 3.20)
project(himax_tsacore VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(CTest)
include(GNUInstallDirs)

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
add_library(himax-tsacore STATIC ${TSACORE_SOURCES}
    "${TSACORE}/HimaxAfeAdapter.cpp" "${TSACORE}/TsaCoreSolver.cpp"
    native-frame.cpp recording.cpp)
target_include_directories(himax-tsacore
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${TSACORE}" "${SOLVERS}"
        "${UPSTREAM}/Common/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms"
    PRIVATE "${TSACORE}/src")
target_compile_options(himax-tsacore PRIVATE
    -fno-strict-aliasing -fwrapv -ffp-contract=off -ftrivial-auto-var-init=zero)
target_compile_definitions(himax-tsacore PUBLIC EGOTOUCH_DIAG=0)
if(HIMAX_PROFILE)
    target_compile_definitions(himax-tsacore PRIVATE TSACORE_PROFILE)
endif()

add_executable(himax-replay replay.cpp)
target_link_libraries(himax-replay PRIVATE himax-tsacore)
target_compile_options(himax-replay PRIVATE -Wall -Wextra -Werror)

add_executable(himax-touchd touchd.cpp v4l2-capture.cpp input.cpp)
target_include_directories(himax-touchd PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms")
target_link_libraries(himax-touchd PRIVATE himax-tsacore)
target_compile_options(himax-touchd PRIVATE -Wall -Wextra -Werror)
install(TARGETS himax-touchd himax-replay RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
configure_file(himax-touchd.service.in himax-touchd.service @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/himax-touchd.service"
    DESTINATION lib/systemd/system)
install(FILES 70-himax-raw.rules DESTINATION lib/udev/rules.d)
install(FILES himax-touchd.modules.conf DESTINATION lib/modules-load.d
    RENAME himax-touchd.conf)

if(BUILD_TESTING)
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
    add_executable(himax-otp-test ../tests/hx-otp-test.c
        ../touchscreen-hx83121a-dkms/hx-otp.c)
    target_include_directories(himax-otp-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../touchscreen-hx83121a-dkms")
    target_compile_options(himax-otp-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME otp-protocol COMMAND himax-otp-test)
endif()
