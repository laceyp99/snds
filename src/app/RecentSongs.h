#pragma once

#include <juce_core/juce_core.h>

#include <optional>
#include <vector>

namespace snds
{
/** One song the user opened lately: only a reference, never mix or playback state. `key` names
    the cache entry; `sourcePath` is where the original was last seen (it may have moved). */
struct RecentSong
{
    juce::String key;
    juce::String title;
    juce::String sourcePath;
};

/** The list of recent songs, newest first, capped. Pure list logic; Settings stores it. */
namespace recent
{
constexpr size_t maxEntries = 20;

/** Puts `song` first, replacing any entry with the same key, and drops the oldest past the cap. */
std::vector<RecentSong> withRecorded (std::vector<RecentSong> list, RecentSong song);

std::vector<RecentSong> without (std::vector<RecentSong> list, const juce::String& key);

std::optional<RecentSong> find (const std::vector<RecentSong>& list, const juce::String& key);
} // namespace recent
} // namespace snds
