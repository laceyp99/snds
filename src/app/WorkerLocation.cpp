#include "app/WorkerLocation.h"

namespace snds
{
juce::StringArray WorkerLocation::commandFor (const juce::String& workerCommand) const
{
    return { pythonExecutable().getFullPathName(), "-I", "-m", "snds_worker", workerCommand };
}

WorkerLocation WorkerLocation::resolve (const juce::File& commandLineRoot,
                                        const juce::String& environmentRoot,
                                        const juce::File& workingDirectory,
                                        const juce::File& buildDefault)
{
    if (commandLineRoot != juce::File())
        return { commandLineRoot, "--worker-root" };

    const auto fromEnvironment = environmentRoot.trim().trimCharactersAtStart ("\"").trimCharactersAtEnd ("\"").trim();

    if (fromEnvironment.isNotEmpty())
        return { workingDirectory.getChildFile (fromEnvironment), rootVariable };

    return { buildDefault, "build default" };
}

WorkerLocation WorkerLocation::fromEnvironment (const juce::File& commandLineRoot)
{
    // Source-first alpha: the build knows its own checkout. SNDS_DEFAULT_WORKER_ROOT is set on
    // this file only, by CMake.
    const juce::File buildDefault (juce::String (juce::CharPointer_UTF8 (SNDS_DEFAULT_WORKER_ROOT)));

    return resolve (commandLineRoot,
                    juce::SystemStats::getEnvironmentVariable (rootVariable, {}),
                    juce::File::getCurrentWorkingDirectory(),
                    buildDefault);
}
} // namespace snds
