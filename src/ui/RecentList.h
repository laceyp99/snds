#pragma once

#include "app/CacheStore.h"
#include "app/RecentSongs.h"
#include "ui/ImportPanel.h"

#include <vector>

namespace snds
{
/** The rows of the recent list. `originals[i]` is where the original of `recents[i]` was last
    seen (empty if unknown). A row's state comes from two facts only: is there a playable cache
    entry for its key, and does the original file exist. A playable entry always wins, so a song
    that is already prepared is never prepared again just because its original moved. Reads the
    disk (file existence), so call it off the message thread. */
std::vector<LibraryItem> buildRecentItems (const std::vector<RecentSong>& recents,
                                           const std::vector<juce::File>& originals,
                                           const std::vector<CacheEntry>& playable);
} // namespace snds
