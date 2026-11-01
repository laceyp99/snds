# Pinned JUCE acquisition and the shared-module pattern used by snds_core.
include_guard(GLOBAL)

include(FetchContent)

# JUCE 8.0.15 (tag 8.0.15). The archive URL names the full commit and the SHA-256 makes the
# download immutable: CMake rejects any archive whose content differs. Evidence and the
# GIT_REPOSITORY fallback are in spikes/juce-fetch/README.md. For offline configures, point
# FETCHCONTENT_SOURCE_DIR_JUCE at an extracted copy of the same commit.
set(SNDS_JUCE_VERSION 8.0.15)
set(SNDS_JUCE_COMMIT 91ad83ae34a81e0833b1a2b0866f54846370ae53)
set(SNDS_JUCE_SHA256 04f8d5055382582c757be9da069ea98338005f98248facd9c2804435ac853e70)

FetchContent_Declare(JUCE
    URL "https://github.com/juce-framework/JUCE/archive/${SNDS_JUCE_COMMIT}.tar.gz"
    URL_HASH SHA256=${SNDS_JUCE_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_MakeAvailable(JUCE)

# Compiles the given JUCE modules into <target>, a static library, and re-exports their
# include directories and compile definitions so that executables linking <target> can use
# the JUCE headers without compiling the module sources again.
#
# JUCE modules are INTERFACE libraries that inject their sources into every target linking
# them, so they are linked PRIVATE here and must never be linked into the executables
# directly. This mirrors how juce_add_plugin shares its "shared code" library.
function(snds_share_juce_modules target)
    target_link_libraries(${target} PRIVATE ${ARGN})
    target_include_directories(${target} INTERFACE $<TARGET_PROPERTY:${target},INCLUDE_DIRECTORIES>)
    target_compile_definitions(${target} INTERFACE $<TARGET_PROPERTY:${target},COMPILE_DEFINITIONS>)
endfunction()
