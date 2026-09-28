#include "app/AppPaths.h"

namespace snds
{

AppPaths::AppPaths (juce::File rootDirectory)
    : root (std::move (rootDirectory))
{
    jassert (root != juce::File());
}

AppPaths AppPaths::forCurrentUser()
{
    const auto localAppData = juce::File::getSpecialLocation (juce::File::windowsLocalAppData);
    const auto overrideValue = juce::SystemStats::getEnvironmentVariable (dataRootOverrideVariable, {});
    return AppPaths (resolveRoot (localAppData, overrideValue));
}

juce::File AppPaths::resolveRoot (const juce::File& localAppDataDirectory, const juce::String& overrideValue)
{
    const auto trimmed = overrideValue.trim().unquoted();

    if (trimmed.isNotEmpty() && juce::File::isAbsolutePath (trimmed))
        return juce::File (trimmed);

    return localAppDataDirectory.getChildFile (rootFolderName);
}

juce::File AppPaths::getDirectory (AppDirectory directory) const
{
    return root.getChildFile (getFolderName (directory));
}

juce::Result AppPaths::ensureDirectory (AppDirectory directory) const
{
    const auto folder = getDirectory (directory);

    if (folder.isDirectory())
        return juce::Result::ok();

    if (folder.exists())
        return juce::Result::fail ("Cannot create " + folder.getFullPathName() + ": a file with that name exists.");

    const auto created = folder.createDirectory();

    if (created.failed())
        return juce::Result::fail ("Cannot create " + folder.getFullPathName() + ": " + created.getErrorMessage());

    return juce::Result::ok();
}

juce::Result AppPaths::ensureAllDirectories() const
{
    for (const auto directory : allAppDirectories)
    {
        const auto result = ensureDirectory (directory);

        if (result.failed())
            return result;
    }

    return juce::Result::ok();
}

const char* AppPaths::getFolderName (AppDirectory directory) noexcept
{
    switch (directory)
    {
        case AppDirectory::settings:
            return "settings";
        case AppDirectory::cache:
            return "cache";
        case AppDirectory::jobs:
            return "jobs";
        case AppDirectory::logs:
            return "logs";
        case AppDirectory::models:
            return "models";
    }

    jassertfalse;
    return "unknown";
}

} // namespace snds
