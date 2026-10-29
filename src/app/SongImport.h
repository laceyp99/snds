#pragma once

#include "model/SongSource.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <memory>
#include <optional>

namespace snds
{

/** Why a file was rejected. Stable identifiers for logs and tests; the user-facing text is in ImportError. */
enum class ImportErrorCode
{
    fileNotFound,
    unreadable,
    unsupportedFileType,
    formatMismatch, ///< The content is a different format than the extension claims.
    unsupportedEncoding,
    unsupportedBitDepth,
    tooManyChannels,
    sampleRateOutOfRange,
    tooLong,
    noAudio,
    truncated,
    corrupt,
};

const char* toString (ImportErrorCode code) noexcept;

struct ImportError
{
    ImportErrorCode code = ImportErrorCode::corrupt;
    juce::String message; ///< What is wrong, in user terms, naming the file.
    juce::String action;  ///< What the user can do about it.
};

struct ImportResult
{
    std::optional<model::SourceInfo> source;
    ImportError error; ///< Only meaningful when source is empty.

    bool wasOk() const noexcept { return source.has_value(); }
};

/** The alpha source contract. */
struct SourceLimits
{
    static constexpr int maxChannels = 2;
    static constexpr double minSampleRate = 8000.0;
    static constexpr double maxSampleRate = 192000.0;
    static constexpr int maxSeconds = 30 * 60; ///< Matches the worker's MAX_SOURCE_SECONDS.
};

/** juce::FileChooser patterns for supported sources. */
inline constexpr const char* sourceFilePatterns = "*.wav;*.wave;*.mp3";

bool hasSupportedSourceExtension (const juce::File& file);

/**
    Validates a file against the alpha source contract and describes it.

    Accepted: WAV (RIFF or RF64, plain or WAVE_FORMAT_EXTENSIBLE) with 8/16/24/32-bit integer PCM or
    32/64-bit float samples, and MP3 as MPEG-1 Layer III (32, 44.1, or 48 kHz); mono or stereo;
    8 kHz to 192 kHz; at most 30 minutes long. Everything else is rejected with an actionable ImportError.

    MPEG-2/2.5 MP3s (24 kHz and below) and MPEG Layer I/II are rejected: JUCE's decoder assumes 1152
    samples per frame, so their length and seek positions would be wrong.

    The check ends with a short decode from the start of the file, so an accepted file opens with
    createSourceReader(). Blocks on file I/O: call it off the message thread.
*/
ImportResult inspectSource (const juce::File& file);

/**
    Opens a reader for a source that inspectSource() accepted, or returns nullptr if it no longer
    opens. MP3 readers report the exact scanned length instead of JUCE's size-based estimate.
    Blocks on file I/O.
*/
std::unique_ptr<juce::AudioFormatReader> createSourceReader (const model::SourceInfo& source);

} // namespace snds
