# Warning policy for SNDS-owned sources.
include_guard(GLOBAL)

option(SNDS_WARNINGS_AS_ERRORS "Treat compiler warnings in SNDS-owned sources as errors" ON)

# Adds SNDS-owned sources to <target>. Every SNDS target gets JUCE's recommended warning flags
# (/W4 on MSVC) through snds_core. Only the sources added here escalate warnings to errors:
# the JUCE module sources compiled inside snds_core are not ours to fix, and a JUCE or compiler
# upgrade should not break the build over them.
function(snds_target_sources target)
    target_sources(${target} PRIVATE ${ARGN})

    if(SNDS_WARNINGS_AS_ERRORS)
        if(MSVC)
            set(werror /WX)
        else()
            set(werror -Werror)
        endif()

        set_property(SOURCE ${ARGN} TARGET_DIRECTORY ${target}
            APPEND PROPERTY COMPILE_OPTIONS ${werror})
    endif()
endfunction()
