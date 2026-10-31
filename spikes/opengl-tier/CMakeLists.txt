# spike-opengl-tier: GLSL through juce::OpenGLContext with CPU and static fallbacks.
cmake_minimum_required(VERSION 3.22...4.3)

project(SndsOpenGLTierSpike VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# Same pin as spikes/juce-fetch: JUCE 8.0.15, immutable commit archive + SHA-256.
FetchContent_Declare(JUCE
    URL "https://github.com/juce-framework/JUCE/archive/91ad83ae34a81e0833b1a2b0866f54846370ae53.tar.gz"
    URL_HASH SHA256=04f8d5055382582c757be9da069ea98338005f98248facd9c2804435ac853e70
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_MakeAvailable(JUCE)

# Two executables from the same sources: the default one lets Windows choose
# the GPU (normally the integrated GPU on hybrid laptops); the dGPU one exports
# NvOptimusEnablement / AmdPowerXpressRequestHighPerformance.
function(snds_add_opengl_tier_app target prefer_discrete_gpu)
    juce_add_gui_app(${target}
        PRODUCT_NAME "${target}"
        COMPANY_NAME "SNDS"
        NEEDS_WEB_BROWSER FALSE
        NEEDS_CURL FALSE)

    target_sources(${target} PRIVATE
        src/ArtworkView.cpp
        src/Main.cpp)

    target_compile_definitions(${target} PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        # Shader failures are handled by falling back; don't assert in Debug.
        JUCE_DONT_ASSERT_ON_GLSL_COMPILE_ERROR=1
        SNDS_PREFER_DISCRETE_GPU=${prefer_discrete_gpu}
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:${target},JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:${target},JUCE_VERSION>")

    target_link_libraries(${target}
        PRIVATE
            juce::juce_gui_basics
            juce::juce_opengl
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
endfunction()

snds_add_opengl_tier_app(SndsOpenGLTier 0)
snds_add_opengl_tier_app(SndsOpenGLTierDgpu 1)
