#include "app/DataPaths.h"

namespace snds
{
juce::File DataPaths::modelFile() const
{
    return modelsDirectory().getChildFile ("htdemucs").getChildFile ("955717e8-8726e21a.th");
}

std::optional<DataPaths> DataPaths::resolve (const juce::String& sndsHome,
                                             const juce::String& localAppData,
                                             const juce::File& workingDirectory)
{
    const auto custom = sndsHome.trim().trimCharactersAtStart ("\"").trimCharactersAtEnd ("\"").trim();

    if (custom.isNotEmpty())
        return DataPaths { workingDirectory.getChildFile (custom) };

    const auto base = localAppData.trim();

    if (base.isEmpty())
        return std::nullopt;

    return DataPaths { workingDirectory.getChildFile (base).getChildFile ("snds") };
}

std::optional<DataPaths> DataPaths::fromEnvironment()
{
    return resolve (juce::SystemStats::getEnvironmentVariable (homeVariable, {}),
                    juce::SystemStats::getEnvironmentVariable ("LOCALAPPDATA", {}),
                    juce::File::getCurrentWorkingDirectory());
}
} // namespace snds
