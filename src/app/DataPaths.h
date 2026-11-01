#pragma once

#include <juce_core/juce_core.h>

#include <optional>

namespace snds
{
/** SNDS data directory layout.

    The worker (worker/src/snds_worker/paths.py) resolves the same locations with the same
    rules. Resolving never touches the disk; whoever writes a file creates its folder.
*/
struct DataPaths
{
    static constexpr auto homeVariable = "SNDS_HOME";

    juce::File home;

    juce::File settingsFile() const { return home.getChildFile ("settings.json"); }
    juce::File cacheDirectory() const { return home.getChildFile ("cache"); }
    juce::File jobsDirectory() const { return home.getChildFile ("jobs"); }
    juce::File logsDirectory() const { return home.getChildFile ("logs"); }
    juce::File modelsDirectory() const { return home.getChildFile ("models"); }

    /** The pinned htdemucs checkpoint, models\htdemucs\955717e8-8726e21a.th. */
    juce::File modelFile() const;

    /** Holds exclusive app ownership of this home on Windows. Keep the stream alive until all
        services stop. Returns null if another app owns the home or it cannot be written.
    */
    std::unique_ptr<juce::FileOutputStream> acquireAppLock() const;

    /** SNDS_HOME if it is set (trimmed, surrounding quotes removed, relative values resolved
        against workingDirectory), otherwise <localAppData>\snds. Returns nullopt if neither
        gives a path.
    */
    static std::optional<DataPaths> resolve (const juce::String& sndsHome,
                                             const juce::String& localAppData,
                                             const juce::File& workingDirectory);

    /** resolve() with this process's environment and working directory. */
    static std::optional<DataPaths> fromEnvironment();
};
} // namespace snds
