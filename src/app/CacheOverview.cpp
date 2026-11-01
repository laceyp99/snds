#include "app/CacheOverview.h"

namespace snds
{
int CacheOverview::playableCount() const noexcept
{
    int count = 0;

    for (const auto& entry : entries)
        count += entry.isPlayable() ? 1 : 0;

    return count;
}

CacheOverview readCacheOverview (const CacheStore& store)
{
    return { store.listEntries(), store.usage() };
}

juce::String formatBytes (juce::int64 bytes)
{
    const auto value = static_cast<double> (juce::jmax<juce::int64> (0, bytes));
    constexpr double kilo = 1024.0;

    if (value >= kilo * kilo * kilo)
        return juce::String (value / (kilo * kilo * kilo), 1) + " GB";

    if (value >= kilo * kilo)
        return (value >= 100.0 * kilo * kilo ? juce::String (juce::roundToInt (value / (kilo * kilo)))
                                             : juce::String (value / (kilo * kilo), 1))
             + " MB"; // String (x, 0) would print every digit

    if (value >= kilo)
        return juce::String (juce::roundToInt (value / kilo)) + " KB";

    return juce::String (static_cast<juce::int64> (value)) + " B";
}
} // namespace snds
