#include "model/SongSource.h"

namespace snds::model
{

const char* toString (SourceFormat format) noexcept
{
    switch (format)
    {
        case SourceFormat::wav:
            return "WAV";
        case SourceFormat::mp3:
            return "MP3";
    }

    return "unknown";
}

double SourceInfo::getLengthSeconds() const noexcept
{
    return sampleRate > 0.0 ? static_cast<double> (lengthInFrames) / sampleRate : 0.0;
}

juce::String SourceInfo::getChannelDescription() const
{
    if (numChannels == 1)
        return "mono";

    if (numChannels == 2)
        return "stereo";

    return juce::String (numChannels) + " channels";
}

juce::String SourceInfo::getSummary() const
{
    return juce::String (toString (format)) + ", " + encoding + ", " + formatSampleRate (sampleRate) + ", "
           + getChannelDescription() + ", " + formatDuration (getLengthSeconds());
}

juce::String formatDuration (double seconds)
{
    const auto total = static_cast<juce::int64> (juce::jmax (0.0, seconds));
    const auto hours = total / 3600;
    const auto minutes = (total / 60) % 60;
    const auto secs = juce::String (total % 60).paddedLeft ('0', 2);

    if (hours > 0)
        return juce::String (hours) + ":" + juce::String (minutes).paddedLeft ('0', 2) + ":" + secs;

    return juce::String (minutes) + ":" + secs;
}

juce::String formatSampleRate (double sampleRate)
{
    // Up to three decimals without trailing zeros: 44.1, 48, 11.025.
    auto text = juce::String (sampleRate / 1000.0, 3);

    if (text.containsChar ('.'))
        text = text.trimCharactersAtEnd ("0").trimCharactersAtEnd (".");

    return text + " kHz";
}

//==============================================================================
bool SourceFingerprint::isValid() const noexcept
{
    return sizeBytes >= 0 && sha256Hex.length() == 64 && sha256Hex.containsOnly ("0123456789abcdef");
}

juce::String SourceFingerprint::getCacheKey() const
{
    return "sha256-" + sha256Hex + "-" + juce::String (sizeBytes);
}

//==============================================================================
const char* toString (ArtworkOrigin origin) noexcept
{
    switch (origin)
    {
        case ArtworkOrigin::embedded:
            return "embedded";
        case ArtworkOrigin::replacement:
            return "replacement";
        case ArtworkOrigin::fallback:
            return "fallback";
    }

    return "unknown";
}

ArtworkChoice::ArtworkChoice (std::optional<Artwork> embeddedIn, Artwork fallbackIn)
    : embedded (std::move (embeddedIn)),
      fallback (std::move (fallbackIn)),
      selected (embedded.has_value() ? ArtworkOrigin::embedded : ArtworkOrigin::fallback)
{
}

const Artwork& ArtworkChoice::getSelected() const noexcept
{
    if (selected == ArtworkOrigin::embedded && embedded.has_value())
        return *embedded;

    if (selected == ArtworkOrigin::replacement && replacement.has_value())
        return *replacement;

    return fallback;
}

void ArtworkChoice::selectEmbedded()
{
    if (! embedded.has_value())
        return;

    selected = ArtworkOrigin::embedded;
    confirmed = false;
}

void ArtworkChoice::selectReplacement (Artwork newReplacement)
{
    newReplacement.origin = ArtworkOrigin::replacement;
    replacement = std::move (newReplacement);
    selected = ArtworkOrigin::replacement;
    confirmed = false;
}

void ArtworkChoice::selectFallback()
{
    selected = ArtworkOrigin::fallback;
    confirmed = false;
}

} // namespace snds::model
