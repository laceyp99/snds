#pragma once

#include "app/RecentSongs.h"

#include <juce_core/juce_core.h>

namespace snds
{
/** App settings stored as JSON in settings.json.

    Loading never fails: a missing, unreadable, invalid, foreign, or newer file means defaults.
    A file that exists but is not usable is backed up once (settings.json.bak, or a numbered
    sibling if that exists) before the first save replaces it. Unknown keys in an SNDS file are
    kept on save. Use from the message thread only.
*/
class Settings
{
public:
    static constexpr auto formatName = "snds-settings";
    static constexpr int formatVersion = 1;

    enum class LoadStatus
    {
        missing,
        loaded,
        unreadable,
        invalid,
        foreign,
        newerVersion
    };

    struct LoadResult
    {
        LoadStatus status = LoadStatus::missing;

        /** Human-readable warnings for the log; empty when the file was missing or fully valid. */
        juce::StringArray warnings;
    };

    explicit Settings (juce::File settingsFile);

    LoadResult load();

    /** Backs up an unusable existing file if needed, then writes atomically. */
    juce::Result save();

    const juce::File& getFile() const noexcept { return file; }

    /** Where save() backed up the previous file, or a default File if it did not. */
    const juce::File& getLastBackup() const noexcept { return lastBackup; }

    /** Use the CPU even when CUDA is available. */
    bool getForceCpu() const;
    void setForceCpu (bool shouldForceCpu);

    /** The audio device the user chose, as the XML AudioDeviceManager::createStateXml() wrote
        it (empty: use the system default). Only the preferred device is stored. */
    juce::String getAudioDeviceState() const;
    void setAudioDeviceState (const juce::String& stateXml);

    /** Recent songs, newest first, at most recent::maxEntries. Entries that are not usable
        references (no valid key) are dropped on load. */
    std::vector<RecentSong> getRecentSongs() const;
    void setRecentSongs (const std::vector<RecentSong>& songs);

private:
    const juce::File file;
    juce::DynamicObject::Ptr values;
    bool backupBeforeSave = false;
    juce::File lastBackup;
};
} // namespace snds
