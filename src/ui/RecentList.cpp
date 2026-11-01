#include "ui/RecentList.h"

#include "app/CacheOverview.h"
#include "ui/SongPanel.h"

namespace snds
{
namespace
{
juce::String describeCached (const CacheEntry& entry)
{
    const auto& song = entry.song.value();

    return formatDuration (song.source.durationSeconds) + "  -  "
         + (song.hasKnownBpm() ? juce::String (juce::roundToInt (song.effectiveBpm())) + " BPM"
                               : juce::String ("no tempo"))
         + "  -  " + formatBytes (entry.sizeBytes);
}
} // namespace

std::vector<LibraryItem> buildRecentItems (const std::vector<RecentSong>& recents,
                                           const std::vector<juce::File>& originals,
                                           const std::vector<CacheEntry>& playable)
{
    std::vector<LibraryItem> items;

    for (size_t i = 0; i < recents.size(); ++i)
    {
        const auto& recentSong = recents[i];
        const auto original = i < originals.size() ? originals[i] : juce::File();
        const auto originalFound = original != juce::File() && original.existsAsFile();

        const CacheEntry* cached = nullptr;

        for (const auto& entry : playable)
            if (entry.key == recentSong.key)
                cached = &entry;

        LibraryItem item;
        item.key = recentSong.key;
        item.sourcePath = original != juce::File() ? original.getFullPathName() : recentSong.sourcePath;

        if (cached != nullptr)
        {
            item.title = cached->song->source.fileName;
            item.state = originalFound ? LibraryItem::State::ready : LibraryItem::State::originalMoved;
            item.subtitle = describeCached (*cached) + (originalFound ? "" : "  -  original not found");
        }
        else
        {
            item.title = recentSong.title.isNotEmpty() ? recentSong.title : original.getFileName();
            item.state = originalFound ? LibraryItem::State::needsPreparing : LibraryItem::State::missing;
            item.subtitle = originalFound ? "Not prepared on this computer. Opening prepares it."
                                          : "Not prepared and the original is missing. Locate it or remove.";
        }

        items.push_back (std::move (item));
    }

    return items;
}
} // namespace snds
