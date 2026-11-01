#pragma once

#include <juce_core/juce_core.h>

namespace snds
{
/** What the app knows about a chosen source file after validation. */
struct SourceInfo
{
    juce::File file;
    juce::String format; // "wav" or "mp3": the `source.container` of song.json
    juce::int64 sizeBytes = 0;
    double sampleRate = 0.0;
    int channels = 0;
    juce::int64 frames = 0;
    double durationSeconds = 0.0;

    int bitsPerSample = 0;      // WAV only; 0 for MP3
    bool floatingPoint = false; // WAV only
};

/** Why a file was rejected. */
enum class ImportProblem
{
    none,
    notFound,
    unsupportedExtension,
    empty,
    unreadable,
    unsupportedCodec,
    unsupportedBitDepth,
    tooManyChannels,
    sampleRateTooHigh,
    sampleRateTooLow,
    zeroLength,
    tooLong
};

struct ImportResult
{
    ImportProblem problem = ImportProblem::none;
    juce::String message; // actionable, shown to the user; empty when accepted
    SourceInfo info;      // meaningful only when accepted

    bool accepted() const noexcept { return problem == ImportProblem::none; }
};

namespace importlimits
{
constexpr int maxChannels = 2;
constexpr double minSampleRate = 8000.0;
constexpr double maxSampleRate = 192000.0;
constexpr double maxDurationSeconds = 30.0 * 60.0;
} // namespace importlimits

/** Checks that a file can be prepared: WAV (PCM 16/24/32-bit or float32) or MP3, mono or
    stereo, 8 to 192 kHz and at most 30 minutes, not empty. Reads headers and the first audio only; it does not
    decode the whole file. Safe to call from a background thread.
*/
ImportResult validateSourceFile (const juce::File& file);
} // namespace snds
