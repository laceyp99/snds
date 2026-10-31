#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace snds::dsp
{
/** The limiter ceiling: -0.3 dBFS. */
inline constexpr float limiterCeilingDb = -0.3f;
inline constexpr float limiterCeiling = 0.96605088f; // 10^(-0.3 / 20)

/** Master gain followed by a transparent safety limiter.

    The mix is summed in float with no internal scaling, so four full-scale stems plus effects may
    exceed 0 dBFS inside the engine without clipping; this stage is the one place that brings it
    back under the ceiling.

    Master gain (-inf..+6 dB, smoothed in the linear domain) runs first, then the limiter: a
    zero-latency, stereo-linked peak limiter. It leaves everything at or below the ceiling
    untouched, bit for bit. A sample that would exceed the ceiling is scaled to it instantly (so
    the output can never exceed it, and there is no lookahead delay to misalign the mix with the
    original), and the gain then recovers exponentially over 100 ms and snaps back to exactly 1.
    Non-finite samples are replaced with silence.

    Audio thread: prepare/setGain/reset/process do not allocate or lock.
*/
class MasterStage
{
public:
    struct BlockInfo
    {
        float inputPeak = 0.0f; // after master gain, before the limiter
        float minGain = 1.0f;   // the lowest limiter gain in the block (1 = idle)
    };

    MasterStage() noexcept;

    void prepare (double sampleRate) noexcept;
    void setGainDb (float decibels) noexcept;

    /** Limiter gain back to 1, master gain snapped to its target. */
    void reset() noexcept;

    /** Stereo in -> stereo out, in place allowed. */
    BlockInfo process (const float* const* in, float* const* out, int numFrames) noexcept;

    float getLimiterGain() const noexcept { return limiterGain; }

private:
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gain;
    float limiterGain = 1.0f;
    float releaseCoefficient = 0.001f;
};
} // namespace snds::dsp
