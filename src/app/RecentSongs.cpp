#include "app/RecentSongs.h"

#include <algorithm>

namespace snds
{
namespace recent
{
std::vector<RecentSong> withRecorded (std::vector<RecentSong> list, RecentSong song)
{
    list = without (std::move (list), song.key);
    list.insert (list.begin(), std::move (song));

    if (list.size() > maxEntries)
        list.resize (maxEntries);

    return list;
}

std::vector<RecentSong> without (std::vector<RecentSong> list, const juce::String& key)
{
    list.erase (std::remove_if (list.begin(), list.end(), [&] (const RecentSong& item) { return item.key == key; }),
                list.end());
    return list;
}

std::optional<RecentSong> find (const std::vector<RecentSong>& list, const juce::String& key)
{
    const auto it = std::find_if (list.begin(), list.end(), [&] (const RecentSong& item) { return item.key == key; });
    return it != list.end() ? std::optional<RecentSong> (*it) : std::nullopt;
}
} // namespace recent
} // namespace snds
