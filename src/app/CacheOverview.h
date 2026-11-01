#pragma once

#include "app/CacheStore.h"

#include <vector>

namespace snds
{
/** What is on disk under SNDS_HOME, for the cache view: every cache folder (playable, incompatible
    or corrupt) and the space used by songs and by temporary job folders. */
struct CacheOverview
{
    std::vector<CacheEntry> entries; // playable newest first, then the others
    DiskUsage usage;

    int playableCount() const noexcept;
    int unusableCount() const noexcept { return static_cast<int> (entries.size()) - playableCount(); }
};

/** Reads the cache folder. Touches the disk: call off the message thread. */
CacheOverview readCacheOverview (const CacheStore& store);

/** "340 MB", "1.2 GB", "12 KB": a size in the unit a person would pick. */
juce::String formatBytes (juce::int64 bytes);
} // namespace snds
