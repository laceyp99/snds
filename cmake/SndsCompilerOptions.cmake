# Compiler options for SNDS-owned code (not applied to JUCE module sources).

include_guard(GLOBAL)

add_library(snds_compiler_options INTERFACE)
add_library(snds::compiler_options ALIAS snds_compiler_options)

target_link_libraries(snds_compiler_options INTERFACE juce::juce_recommended_warning_flags)

if(MSVC)
    target_compile_options(snds_compiler_options INTERFACE
        /utf-8
        /permissive-
        /Zc:__cplusplus
        $<$<BOOL:${SNDS_WARNINGS_AS_ERRORS}>:/WX>)
else()
    target_compile_options(snds_compiler_options INTERFACE
        $<$<BOOL:${SNDS_WARNINGS_AS_ERRORS}>:-Werror>)
endif()

# Every SNDS executable lands in <build>/bin/<Config>/ so scripts can find them predictably.
set(SNDS_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

function(snds_set_runtime_output target)
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${SNDS_RUNTIME_OUTPUT_DIRECTORY}")
endfunction()
