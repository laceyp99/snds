#include "app/AppOptions.h"

namespace snds
{
namespace
{
// Matches "--name value" and "--name=value". Returns true if arguments[index] is the option,
// advancing index past a separate value; value is empty if none was given.
bool takeValueOption (const juce::StringArray& arguments, int& index, const juce::String& name, juce::String& value)
{
    const auto argument = arguments[index].trim();

    if (argument != name && ! argument.startsWith (name + "="))
        return false;

    value = {};

    if (argument.containsChar ('='))
        value = argument.fromFirstOccurrenceOf ("=", false, false);
    else if (index + 1 < arguments.size() && ! arguments[index + 1].trim().startsWith ("--"))
        value = arguments[++index];

    value = value.trim().unquoted().trim();
    return true;
}
} // namespace

AppOptions AppOptions::parse (const juce::StringArray& arguments, const juce::File& workingDirectory)
{
    AppOptions options;

    for (int i = 0; i < arguments.size(); ++i)
    {
        const auto argument = arguments[i].trim();
        juce::String value;

        if (argument.isEmpty())
            continue;

        if (argument == "--smoke-exit")
        {
            options.smokeExit = true;
        }
        else if (argument == "--smoke-exit-after-readiness")
        {
            options.smokeExitAfterReadiness = true;
        }
        else if (argument == "--no-audio")
        {
            options.noAudio = true;
        }
        else if (takeValueOption (arguments, i, "--open-song", value))
        {
            if (value.isEmpty())
                options.problems.add ("--open-song needs a cache key");
            else
                options.openSong = value;
        }
        else if (takeValueOption (arguments, i, "--export-diagnostics", value))
        {
            if (value.isEmpty())
                options.problems.add ("--export-diagnostics needs a file path");
            else
                options.exportDiagnostics = workingDirectory.getChildFile (value);
        }
        else if (takeValueOption (arguments, i, "--worker-root", value))
        {
            if (value.isEmpty())
                options.problems.add ("--worker-root needs a folder path");
            else
                options.workerRoot = workingDirectory.getChildFile (value);
        }
        else if (takeValueOption (arguments, i, "--readiness-timeout", value))
        {
            const auto seconds = value.getIntValue();

            if (! value.containsOnly ("0123456789") || value.isEmpty() || seconds < 1 || seconds > 3600)
                options.problems.add ("--readiness-timeout needs a whole number of seconds from 1 to 3600");
            else
                options.readinessTimeoutSeconds = seconds;
        }
        else
        {
            options.problems.add ("Unrecognised argument: " + argument);
        }
    }

    return options;
}
} // namespace snds
