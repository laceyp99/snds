#pragma once

#include <juce_core/juce_core.h>

namespace snds
{
/** Command-line options for the SNDS app.

    Malformed or unknown arguments are collected in `problems` instead of stopping the app, so
    a stray argument never prevents startup; automation flags treat any problem as a failure.
*/
struct AppOptions
{
    static constexpr int defaultReadinessTimeoutSeconds = 90;

    /** --smoke-exit: open the main window, then quit with 0 if it appeared. */
    bool smokeExit = false;

    /** --smoke-exit-after-readiness: like --smoke-exit, but wait for the first readiness check
        and log its result. Exit code 0 ready, 10 not set up, 11 model missing, 12 error, or 1 if
        the window, argument, or JUCE checks failed. Takes precedence over --smoke-exit.
    */
    bool smokeExitAfterReadiness = false;

    /** --no-audio: never open an audio device (the player then reports no output device). For
        automation and screenshots. */
    bool noAudio = false;

    /** --open-song <cache key>: open that prepared song when the window appears (for automation).
        Never starts playback. */
    juce::String openSong;

    /** --export-diagnostics <zip file>: once the first readiness check is done and the window has
        been up a few seconds, write the same zip as "Export diagnostics..." there and quit (0 on
        success, 1 on failure). For automation; no dialog is shown. */
    juce::File exportDiagnostics;

    /** --worker-root <folder> or --worker-root=<folder>; a default File when not given. */
    juce::File workerRoot;

    /** --readiness-timeout <seconds>, 1 to 3600. */
    int readinessTimeoutSeconds = defaultReadinessTimeoutSeconds;

    juce::StringArray problems;

    /** Parses arguments (without the executable). Relative paths resolve against workingDirectory. */
    static AppOptions parse (const juce::StringArray& arguments,
                             const juce::File& workingDirectory = juce::File::getCurrentWorkingDirectory());
};
} // namespace snds
