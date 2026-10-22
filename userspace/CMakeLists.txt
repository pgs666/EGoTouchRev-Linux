cmake_minimum_required(VERSION 3.20)
project(himax_tsacore VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(CTest)

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
    native-frame.cpp)
target_include_directories(himax-tsacore
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${TSACORE}" "${SOLVERS}"
        "${UPSTREAM}/Common/include"
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

if(BUILD_TESTING)
    add_executable(himax-native-test tests/native-test.cpp)
    target_link_libraries(himax-native-test PRIVATE himax-tsacore)
    target_compile_options(himax-native-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME native-frame COMMAND himax-native-test)
endif()
