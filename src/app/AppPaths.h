#pragma once

#include <juce_core/juce_core.h>

#include <array>

namespace snds
{

/** Per-user data areas below the SNDS data root (normally %LOCALAPPDATA%\snds). */
enum class AppDirectory
{
    settings, ///< App settings, recent files, device choice. Never mix state.
    cache,    ///< Validated song packages (derived artifacts only, never user source audio).
    jobs,     ///< Temporary per-job working directories used before atomic cache publication.
    logs,     ///< Rotating app and worker logs.
    models,   ///< Pinned separation model artifacts.
};

inline constexpr std::array<AppDirectory, 5> allAppDirectories {
    AppDirectory::settings, AppDirectory::cache, AppDirectory::jobs, AppDirectory::logs, AppDirectory::models,
};

/**
    Resolves the SNDS per-user directories.

    Path resolution is pure: the directory getters never touch the filesystem. Directories are
    created on demand through ensureDirectory(), so a fresh machine only gains the folders that are
    actually used. The root is injectable so tests never write into the real profile.
*/
class AppPaths
{
public:
    /** Environment variable that overrides the data root. Must be an absolute path. */
    static constexpr const char* dataRootOverrideVariable = "SNDS_DATA_DIR";

    /** Folder created below %LOCALAPPDATA%. */
    static constexpr const char* rootFolderName = "snds";

    explicit AppPaths (juce::File rootDirectory);

    /** Resolves the root for the current user from the environment (see resolveRoot()). */
    static AppPaths forCurrentUser();

    /**
        Pure root resolution: an absolute override wins; otherwise <localAppData>\snds.
        Empty or relative override values are ignored.
    */
    static juce::File resolveRoot (const juce::File& localAppDataDirectory, const juce::String& overrideValue);

    const juce::File& getRoot() const noexcept { return root; }

    /** Returns the directory path without creating it. */
    juce::File getDirectory (AppDirectory directory) const;

    /** Creates the directory (and missing parents) if needed. */
    juce::Result ensureDirectory (AppDirectory directory) const;

    /** Creates every SNDS directory, stopping at the first failure. */
    juce::Result ensureAllDirectories() const;

    static const char* getFolderName (AppDirectory directory) noexcept;

private:
    juce::File root;
};

} // namespace snds
