#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <optional>

namespace snds
{
/** The `song.json` of a prepared package (cacheSchema 1). See docs/protocol.md. */
struct SongMetadata
{
    static constexpr int cacheSchema = 1;
    static constexpr int stemCount = 4;

    /** Stem names in file order: drums, bass, other, vocals. Each stem is `<name>.wav`. */
    static const std::array<const char*, stemCount>& stemNames();

    struct Source
    {
        juce::String fileName;
        juce::String path; // where the file was when the package was prepared
        juce::int64 sizeBytes = 0;
        juce::String sha256;
        juce::String container; // "wav" or "mp3"
        int sampleRate = 0;
        int channels = 0;
        juce::int64 frames = 0;
        double durationSeconds = 0.0;
    };

    struct Stem
    {
        juce::String file;
        juce::int64 frames = 0;
        int sampleRate = 0;
        int channels = 0;
        juce::String sampleFormat;
        double peak = 0.0;
        juce::String sha256; // absent in older packages: no trusted content checksum
        juce::int64 sizeBytes = 0;
    };

    struct Model
    {
        juce::String name;
        juce::String signature;
        juce::String file;
        juce::String sha256;
    };

    struct BpmOverride
    {
        double value = 0.0;
        juce::String mode; // "numeric", "half", or "double"
    };

    struct Bpm
    {
        std::optional<double> value; // empty when detection failed (bpm.value is null)
        juce::String error;          // why detection failed; empty on success
        double confidence = 0.0;
        int beats = 0;
        juce::Array<double> alternatives;
        juce::String analyzerName;
        juce::String analyzerVersion;
        juce::String checkpointSha256;
        std::optional<BpmOverride> userOverride;
    };

    struct Artwork
    {
        juce::String file;   // empty when there is none
        juce::String origin; // "embedded", "file", or "fallback"
    };

    struct Worker
    {
        juce::String version;
        int protocol = 0;
        juce::String torch;
        juce::String device;
    };

    Source source;
    std::array<Stem, stemCount> stems;
    Model model;
    Bpm bpm;
    Artwork artwork;
    Worker worker;
    juce::String preparedAt; // UTC ISO-8601

    /** Used as the tempo until the user sets one when detection found none. */
    static constexpr double defaultBpm = 120.0;

    /** True if there is a tempo to trust: the user's correction or a detected value. */
    bool hasKnownBpm() const noexcept { return bpm.userOverride.has_value() || bpm.value.has_value(); }

    /** The BPM to use: the user's correction if there is one, else the detected value, else
        defaultBpm (check hasKnownBpm() to flag that case in the UI).
    */
    double effectiveBpm() const noexcept
    {
        return bpm.userOverride ? bpm.userOverride->value : bpm.value.value_or (defaultBpm);
    }

    /** `<sha256>-<sizeBytes>`: the package folder name. */
    static juce::String cacheKey (const juce::String& sha256, juce::int64 sizeBytes);
    juce::String cacheKey() const { return cacheKey (source.sha256, source.sizeBytes); }
};

/** What this build can play. A package prepared with anything else is "incompatible". */
struct SongCompatibility
{
    juce::String modelName = "htdemucs";
    juce::String modelSignature = "955717e8";
    int protocol = 1;
};

struct SongLoadResult
{
    enum class Status
    {
        complete,     // valid and playable
        incompatible, // well-formed but prepared by another schema, model, or protocol
        corrupt       // unreadable, malformed, not complete, or its files are missing
    };

    Status status = Status::corrupt;
    juce::String reason;   // empty when complete
    SongMetadata metadata; // meaningful only when complete

    bool isComplete() const noexcept { return status == Status::complete; }

    static juce::String statusName (Status status);
};

/** Validates the text of a song.json. Never throws. */
SongLoadResult parseSongMetadata (const juce::String& json, const SongCompatibility& compatibility = {});

/** Loads `<packageDir>\song.json` and additionally requires the folder name to equal the cache
    key and every stem file (and the artwork file, if named) to exist.
*/
SongLoadResult loadSongPackage (const juce::File& packageDir, const SongCompatibility& compatibility = {});
} // namespace snds
