# Pinned JUCE acquisition and the shared JUCE static library.
#
# JUCE is fetched as an immutable commit archive verified by SHA256, so configure is reproducible
# without git submodules. To build against a local JUCE checkout instead (e.g. offline), configure
# with -DFETCHCONTENT_SOURCE_DIR_JUCE=<path-to-JUCE>; the pinned values below remain the reference.
#
# Shared-code pattern: JUCE modules are INTERFACE libraries that add their sources to every target
# that links them. Linking them directly into the app, the test runner, and the spikes would compile
# JUCE once per executable and makes a shared SNDS library impossible (duplicate symbols). Instead,
# `snds_juce` links the modules PRIVATE (so their sources compile exactly once, into this archive) and
# re-exports the modules' include directories and compile definitions through its INTERFACE. Every
# SNDS target links `snds::juce` and therefore sees one consistent JUCE configuration.

include_guard(GLOBAL)
include(FetchContent)

set(SNDS_JUCE_VERSION "9.0.2")
set(SNDS_JUCE_COMMIT "72782788ce18c2d4d760b28e0921d6ffc6431102")
set(SNDS_JUCE_SHA256 "28ec8ae0626a4d1c6ca8c32ad13f95868b147779d4f3df4a7868f333a38e0551")

FetchContent_Declare(juce
    URL "https://github.com/juce-framework/JUCE/archive/${SNDS_JUCE_COMMIT}.tar.gz"
    URL_HASH "SHA256=${SNDS_JUCE_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)

FetchContent_MakeAvailable(juce)

get_directory_property(snds_configured_juce_version DIRECTORY "${juce_SOURCE_DIR}" DEFINITION JUCE_VERSION)

if(NOT snds_configured_juce_version STREQUAL SNDS_JUCE_VERSION)
    message(WARNING "Expected JUCE ${SNDS_JUCE_VERSION} but configured JUCE ${snds_configured_juce_version}. "
        "Check FETCHCONTENT_SOURCE_DIR_JUCE.")
endif()

message(STATUS "SNDS: JUCE ${snds_configured_juce_version} from ${juce_SOURCE_DIR}")

add_library(snds_juce STATIC)
add_library(snds::juce ALIAS snds_juce)

target_link_libraries(snds_juce
    PRIVATE
        juce::juce_core
        juce::juce_cryptography
        juce::juce_events
        juce::juce_data_structures
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_audio_devices
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_opengl
    PUBLIC
        juce::juce_recommended_config_flags)

target_compile_features(snds_juce PUBLIC cxx_std_20)

target_compile_definitions(snds_juce
    PUBLIC
        # Every SNDS executable is a standalone application (no plugin wrappers).
        JUCE_STANDALONE_APPLICATION=1
        # No embedded browser, no libcurl, no network-backed features.
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        # Alpha formats are WAV and MP3 only. MP3 uses JUCE's built-in decoder (no FFmpeg).
        # Windows Media decoding is disabled so MP3 behaviour does not depend on OS codecs.
        JUCE_USE_MP3AUDIOFORMAT=1
        JUCE_USE_WINDOWS_MEDIA_FORMAT=0
        JUCE_USE_FLAC=0
        JUCE_USE_OGGVORBIS=0
        # Plugin hosting is out of scope.
        JUCE_PLUGINHOST_VST3=0
        JUCE_PLUGINHOST_AU=0
        JUCE_PLUGINHOST_LADSPA=0
        JUCE_PLUGINHOST_LV2=0)

# Re-export the (transitive) module definitions and include directories collected above.
# The include directories are SYSTEM so JUCE headers do not trip SNDS warnings-as-errors.
target_compile_definitions(snds_juce INTERFACE
    $<TARGET_PROPERTY:snds_juce,COMPILE_DEFINITIONS>)
target_include_directories(snds_juce SYSTEM INTERFACE
    $<TARGET_PROPERTY:snds_juce,INCLUDE_DIRECTORIES>)

set_target_properties(snds_juce PROPERTIES FOLDER "ThirdParty")
