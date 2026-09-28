cmake_minimum_required(VERSION 3.25)

project(SNDS
    VERSION 0.1.0
    DESCRIPTION "Local visual stem-remix instrument"
    LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    message(WARNING "SNDS alpha supports Windows 11 x64 only; other platforms are unsupported.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(SNDS_BUILD_TESTS "Build the SNDS C++ unit test runner" ON)
option(SNDS_BUILD_SPIKES "Build the Phase 0 spike executables under spikes/cpp" ON)
option(SNDS_WARNINGS_AS_ERRORS "Treat compiler warnings in SNDS-owned code as errors" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(SndsJuce)
include(SndsCompilerOptions)

add_subdirectory(src)

if(SNDS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/cpp)
endif()

if(SNDS_BUILD_SPIKES)
    add_subdirectory(spikes/cpp)
endif()
