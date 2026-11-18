cmake_minimum_required(VERSION 3.20)
project(malgrind LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 UNICODE _UNICODE)
endif()

function(malgrind_add_test name)
    add_executable(test_${name} ${ARGN})
    target_compile_options(test_${name} PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
    add_test(NAME ${name} COMMAND test_${name})
endfunction()

# The PC tool uses C++20 <format>: GCC >= 13, or Clang/libc++ >= 17 (Xcode 15, macOS 13.3+ deployment target).
include(CheckCXXSourceCompiles)
check_cxx_source_compiles("#include <format>\nint main() { return std::format(\"{}\", 1).size() == 1 ? 0 : 1; }" MALGRIND_HAS_FORMAT)
if(NOT MALGRIND_HAS_FORMAT)
    message(FATAL_ERROR "malgrind needs a C++20 standard library with <format> (GCC >= 13, or Xcode 15 / LLVM 17+ on macOS 13.3+)")
endif()

# Platform-agnostic core (no SDK, no heap)
add_subdirectory(core)

# Host tool: `malgrind` command line (frame decoder, symbolizer, live flamegraph server). Linux and macOS.
add_subdirectory(host)

# Host port + demo: a Linux process that profiles itself, to try the pipeline without a board.
# Linux only: it is linked static and non-PIE so that its 32-bit PCs match the ELF (macOS has neither).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(platforms/host)
    add_subdirectory(apps/host_runner)
else()
    message(STATUS "host_runner demo skipped (Linux only); use a board with `malgrind run`")
endif()
