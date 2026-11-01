#pragma once

#include <juce_core/juce_core.h>

namespace snds
{
/** Where the Python worker project lives, and how the app runs it. */
struct WorkerLocation
{
    static constexpr auto rootVariable = "SNDS_WORKER_ROOT";

    juce::File root;

    /** "--worker-root", "SNDS_WORKER_ROOT", or "build default", for logs and diagnostics. */
    juce::String source;

    /** The interpreter of the uv-managed environment that scripts\bootstrap.ps1 creates. */
    juce::File pythonExecutable() const { return root.getChildFile (".venv\\Scripts\\python.exe"); }

    /** The command line for `python -I -m snds_worker <command>`. Isolated mode (-I) ignores
        PYTHON* environment variables and the user site folder, so only the locked environment
        is used.
    */
    juce::StringArray commandFor (const juce::String& workerCommand) const;

    /** --worker-root if given, else SNDS_WORKER_ROOT (relative values resolve against
        workingDirectory), else buildDefault.
    */
    static WorkerLocation resolve (const juce::File& commandLineRoot,
                                   const juce::String& environmentRoot,
                                   const juce::File& workingDirectory,
                                   const juce::File& buildDefault);

    /** resolve() with this process's environment and the worker folder of the source tree this
        app was built from.
    */
    static WorkerLocation fromEnvironment (const juce::File& commandLineRoot);
};
} // namespace snds
