#pragma once

#include <juce_core/juce_core.h>

namespace snds::spike
{
// Version of the JUCE headers this translation unit was compiled against.
inline juce::String compiledJuceVersion()
{
    return juce::String (JUCE_MAJOR_VERSION) + "." + juce::String (JUCE_MINOR_VERSION) + "."
         + juce::String (JUCE_BUILDNUMBER);
}

inline bool juceVersionMatchesPin()
{
    return compiledJuceVersion() == juce::String (SNDS_EXPECTED_JUCE_VERSION);
}

// Stand-in for SNDS logic that the C++ test target exercises without a GUI.
inline double samplesToSeconds (juce::int64 samples, double sampleRate) noexcept
{
    return sampleRate > 0.0 ? static_cast<double> (samples) / sampleRate : 0.0;
}
} // namespace snds::spike
