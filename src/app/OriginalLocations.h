#pragma once

#include <juce_core/juce_core.h>

#include <optional>

namespace snds
{
/** Where the user's original file for each prepared song is, as far as the app has seen.

    The package's own `source.path` is only a hint from preparation time (the original may have
    moved); a path confirmed by importing the file again or by `Locate Original` is remembered
    here, in SNDS_HOME\originals.json, keyed by cache key. Loading never fails: a missing or
    damaged file means no known locations. Use from the message thread only.
*/
class OriginalLocations
{
public:
    static constexpr int formatVersion = 1;

    explicit OriginalLocations (juce::File file);

    std::optional<juce::File> find (const juce::String& cacheKey) const;

    /** Remembers the location and saves atomically. Returns false if the file could not be written. */
    bool remember (const juce::String& cacheKey, const juce::File& original);

    const juce::File& getFile() const noexcept { return file; }

private:
    void load();

    juce::File file;
    juce::StringPairArray locations;
};
} // namespace snds
