# spike-juce-fetch: configure and build a pinned JUCE revision through FetchContent.
#
# The `...4.3` upper bound opts into every policy up to CMake 4.3 (for example
# CMP0168: FetchContent downloads directly instead of through a sub-build).
# JUCE itself declares cmake_minimum_required(VERSION 3.22), so no
# CMAKE_POLICY_VERSION_MINIMUM override is needed with CMake 4.x.
cmake_minimum_required(VERSION 3.22...4.3)

project(SndsJuceFetchSpike VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# JUCE 8.0.15 (git tag 8.0.15, published 2026-07-21). The archive URL names the
# full commit, and the SHA-256 makes the download immutable: CMake refuses any
# archive whose content differs.
set(SNDS_JUCE_VERSION 8.0.15)
set(SNDS_JUCE_COMMIT 91ad83ae34a81e0833b1a2b0866f54846370ae53)
set(SNDS_JUCE_SHA256 04f8d5055382582c757be9da069ea98338005f98248facd9c2804435ac853e70)

FetchContent_Declare(JUCE
    URL "https://github.com/juce-framework/JUCE/archive/${SNDS_JUCE_COMMIT}.tar.gz"
    URL_HASH SHA256=${SNDS_JUCE_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_MakeAvailable(JUCE)

# Header-only code shared by the app and the tests, standing in for SNDS logic
# that tests will exercise without launching the GUI.
add_library(snds_spike_shared INTERFACE)
target_include_directories(snds_spike_shared INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(snds_spike_shared INTERFACE
    SNDS_EXPECTED_JUCE_VERSION="${SNDS_JUCE_VERSION}")

# --- App target ------------------------------------------------------------
juce_add_gui_app(snds_juce_fetch_app
    PRODUCT_NAME "SNDS JUCE Fetch Spike"
    COMPANY_NAME "SNDS"
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

target_sources(snds_juce_fetch_app PRIVATE src/Main.cpp)

target_compile_definitions(snds_juce_fetch_app PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:snds_juce_fetch_app,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:snds_juce_fetch_app,JUCE_VERSION>")

target_link_libraries(snds_juce_fetch_app
    PRIVATE
        snds_spike_shared
        juce::juce_gui_basics
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# --- C++ test target -------------------------------------------------------
juce_add_console_app(snds_juce_fetch_tests
    PRODUCT_NAME "SNDS JUCE Fetch Spike Tests"
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

target_sources(snds_juce_fetch_tests PRIVATE tests/TestMain.cpp)

target_compile_definitions(snds_juce_fetch_tests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)

target_link_libraries(snds_juce_fetch_tests
    PRIVATE
        snds_spike_shared
        juce::juce_core
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

enable_testing()
add_test(NAME snds_juce_fetch_tests COMMAND snds_juce_fetch_tests)
