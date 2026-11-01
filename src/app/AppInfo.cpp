#include "app/AppInfo.h"

namespace snds::appinfo
{
juce::String version()
{
    return SNDS_VERSION_STRING;
}

juce::String pinnedJuceVersion()
{
    return SNDS_JUCE_VERSION_STRING;
}

juce::String compiledJuceVersion()
{
    return juce::String (JUCE_MAJOR_VERSION) + "." + juce::String (JUCE_MINOR_VERSION) + "."
         + juce::String (JUCE_BUILDNUMBER);
}

bool juceMatchesPin()
{
    return compiledJuceVersion() == pinnedJuceVersion()
        && juce::SystemStats::getJUCEVersion() == "JUCE v" + pinnedJuceVersion();
}
} // namespace snds::appinfo
