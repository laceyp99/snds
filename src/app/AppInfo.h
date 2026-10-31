#pragma once

#include <juce_core/juce_core.h>

namespace snds::appinfo
{
inline constexpr auto name = "SNDS";

/** SNDS version from the CMake project, e.g. "0.0.1". */
juce::String version();

/** JUCE version pinned in cmake/SndsJuce.cmake. */
juce::String pinnedJuceVersion();

/** Version of the JUCE headers snds_core was compiled against. */
juce::String compiledJuceVersion();

/** True if both the compiled JUCE headers and the linked JUCE runtime match the pin. */
bool juceMatchesPin();
} // namespace snds::appinfo
