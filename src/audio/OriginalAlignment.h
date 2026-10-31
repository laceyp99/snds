#pragma once

#include "audio/FrameSource.h"
#include "audio/Mp3Gapless.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <array>
#include <functional>
#include <memory>

namespace snds::audio
{
/** How well the original, as it would play, lines up with the sum of the four stems. */
struct AlignmentMeasurement
{
    bool measurable = false;  // false if the chosen window was too quiet to correlate
    int lagFrames = 0;        // original[n + lag] matches stems[n]; 0 means aligned
    double correlation = 0.0; // normalised, -1..1, at that lag
    std::int64_t windowStart = 0;
    int windowFrames = 0;
};

/** Cross-correlates a loud window (up to 5 s) of the original against the sum of the stems,
    over lags of +-`maxLag` frames. Slow (disk and FFT): never call it on the audio or message
    thread. `cancelled` is polled between steps. */
AlignmentMeasurement measureAlignment (FrameSource& original,
                                       const std::array<FrameSource*, 4>& stems,
                                       std::int64_t songFrames,
                                       int sampleRate,
                                       const std::function<bool()>& cancelled = {},
                                       int maxLag = 4800);

/** The outcome of preparing a song's original for Original/Stem Mix A/B. */
struct OriginalPlan
{
    bool usable = false;
    juce::String reason;  // why A/B is disabled; empty when usable
    juce::String summary; // one line for the log: container, offsets, measured lag, correlation

    int leadingSkip = 0; // decoded frames dropped from the start (MP3 delay + decoder delay)
    Mp3Info mp3;
    AlignmentMeasurement measured;
    std::unique_ptr<FrameSource> source; // set when usable; mono duplicated, trimmed, song-length
};

/** Opens the original and works out where it lines up with the stems:

      WAV: no offset.
      MP3: JUCE decodes everything, the worker's decode drops the LAME encoder delay and 529
           decoder samples, so the leading skip is read from the Info tag.

    Then it VERIFIES that with measureAlignment. The original is usable only if the measured
    lag is exactly 0 with a correlation of at least 0.8; otherwise A/B is disabled and `reason`
    says why (an unexplained offset is reported with its measured size).
*/
OriginalPlan planOriginal (const juce::File& originalFile,
                           const juce::String& container, // "wav" or "mp3"
                           juce::AudioFormatManager& formats,
                           std::int64_t songFrames,
                           int sampleRate,
                           const std::array<FrameSource*, 4>& stems,
                           const std::function<bool()>& cancelled = {});
} // namespace snds::audio
