#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

#include <optional>

/**
    Plain data describing the song being imported: the validated source file, its content
    fingerprint, and the artwork chosen for it. Produced by the import pipeline (app/SongImport,
    app/SourceFingerprint, app/EmbeddedArtwork) and consumed by the UI and, later, the cache.

    The source file is referenced in place and never copied.
*/
namespace snds::model
{

enum class SourceFormat
{
    wav,
    mp3,
};

const char* toString (SourceFormat format) noexcept;

/**
    MPEG audio details kept for MP3 sources.

    JUCE's MP3 decoder is not gapless: it outputs every audio frame (1152 samples each), including
    the encoder delay and padding. The worker's decoder (libsndfile/mpg123) trims them when a LAME
    tag is present. The streaming engine needs these values to line up Original A/B with the stems.
*/
struct Mp3Details
{
    juce::int64 audioDataOffset = 0; ///< Byte offset of the first MPEG frame (after any ID3v2 tags).
    juce::int64 audioFrameCount = 0; ///< Frames that decode to audio (a leading Xing/Info frame is excluded).
    int bitrateKbps = 0;             ///< First-frame bitrate for CBR; average bitrate for VBR.
    bool variableBitrate = false;
    bool hasXingHeader = false;
    int encoderDelay = -1;   ///< LAME tag encoder delay in samples; -1 when unknown.
    int encoderPadding = -1; ///< LAME tag end padding in samples; -1 when unknown.
    int resyncCount = 0;     ///< Places where the frame chain broke and had to be re-found (damage).
};

/** Validated facts about one user-selected source file. */
struct SourceInfo
{
    juce::File file;
    SourceFormat format = SourceFormat::wav;
    juce::String encoding; ///< e.g. "24-bit PCM", "64-bit float", "MPEG-1 Layer III, 128 kbps".
    double sampleRate = 0.0;
    int numChannels = 0;
    juce::int64 lengthInFrames = 0; ///< Sample frames the SNDS decoder yields (see Mp3Details for MP3).
    int bitsPerSample = 0;          ///< Stored WAV sample size; 0 for MP3 (decoded to 32-bit float).
    bool floatingPoint = false;     ///< WAV samples are IEEE float.
    juce::int64 fileSizeBytes = 0;
    juce::Time lastModified;
    std::optional<Mp3Details> mp3;

    double getLengthSeconds() const noexcept;

    /** "mono" or "stereo". */
    juce::String getChannelDescription() const;

    /** One line for the UI and logs, e.g. "WAV, 24-bit PCM, 48 kHz, stereo, 3:12". */
    juce::String getSummary() const;
};

/** "3:07", or "1:02:03" for an hour or more. Rounds down to whole seconds. */
juce::String formatDuration (double seconds);

/** "44.1 kHz", "48 kHz", "11.025 kHz". */
juce::String formatSampleRate (double sampleRate);

//==============================================================================
/**
    Content identity of a source file: SHA-256 over every byte plus the byte count. Together they are
    the cache identity, so a moved or renamed file still finds its cache and an edited file never does.
*/
struct SourceFingerprint
{
    juce::String sha256Hex; ///< 64 lowercase hex digits.
    juce::int64 sizeBytes = -1;

    bool isValid() const noexcept;

    /** "sha256:<hex>". */
    juce::String getContentHash() const { return "sha256:" + sha256Hex; }

    /** File-name-safe cache identity: "sha256-<hex>-<size>". */
    juce::String getCacheKey() const;

    bool operator== (const SourceFingerprint&) const = default;
};

//==============================================================================
enum class ArtworkOrigin
{
    embedded,    ///< Found in the source file's metadata.
    replacement, ///< An image file the user chose.
    fallback,    ///< Generated from the file name; always available.
};

const char* toString (ArtworkOrigin origin) noexcept;

struct Artwork
{
    juce::Image image; ///< Software image; safe to read from any thread.
    ArtworkOrigin origin = ArtworkOrigin::fallback;
    juce::String description; ///< e.g. "Embedded front cover (JPEG, 600 x 600)".
    juce::File file;          ///< The replacement image file; empty for other origins.

    bool isValid() const noexcept { return image.isValid(); }
};

/**
    Artwork confirmation state for one import: the embedded candidate (if any), the latest
    replacement (if any), the generated fallback, the current selection, and whether the user
    confirmed it. The embedded artwork is selected initially when present. Changing the selection
    withdraws an earlier confirmation, so preparation only ever starts from a confirmed choice.
*/
class ArtworkChoice
{
public:
    ArtworkChoice() = default;
    ArtworkChoice (std::optional<Artwork> embedded, Artwork fallback);

    const Artwork& getSelected() const noexcept;
    ArtworkOrigin getSelectedOrigin() const noexcept { return selected; }

    bool hasEmbedded() const noexcept { return embedded.has_value(); }
    bool hasReplacement() const noexcept { return replacement.has_value(); }

    /** Does nothing when there is no embedded artwork. */
    void selectEmbedded();
    void selectReplacement (Artwork newReplacement);
    void selectFallback();

    void confirm() noexcept { confirmed = true; }
    bool isConfirmed() const noexcept { return confirmed; }

private:
    std::optional<Artwork> embedded;
    std::optional<Artwork> replacement;
    Artwork fallback;
    ArtworkOrigin selected = ArtworkOrigin::fallback;
    bool confirmed = false;
};

} // namespace snds::model
