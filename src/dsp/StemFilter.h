#pragma once

#include "dsp/MixParameters.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>

namespace snds::dsp
{
/** One stereo cut filter: a 2nd-order Butterworth (12 dB/octave, Q = 1/sqrt2) high-pass or
    low-pass built as a trapezoidal (TPT) state-variable filter.

    The TPT form is unconditionally stable and well behaved when its cutoff moves every sample,
    and the bilinear prewarp puts the -3.01 dB point exactly on the cutoff at every sample rate
    (up to 0.49 x the rate). The cutoff is smoothed exponentially (so a sweep is even across
    octaves) and the coefficients are refreshed every few samples while it moves.

    At its endpoint (low cut at 20 Hz, high cut at 20 kHz) the filter is bypassed: the output is
    the input, bit for bit, and the state is held at zero. Leaving or entering the bypass
    crossfades the filtered signal in or out over the smoothing time, so engaging a cut never
    clicks. Both channels use identical coefficients and arithmetic, so they match exactly.
*/
class CutFilter
{
public:
    enum class Mode
    {
        highPass, // the "low cut"
        lowPass   // the "high cut"
    };

    explicit CutFilter (Mode mode) noexcept;

    void prepare (double sampleRate, float smoothingSeconds) noexcept;

    /** Sets the cutoff in Hz (clamped to the parameter range); the endpoint means bypassed. */
    void setCutoff (float hertz) noexcept;

    /** Snaps to the current target and clears the filter state. */
    void reset() noexcept;

    /** Stereo in -> stereo out, in place allowed. */
    void process (const float* const* in, float* const* out, int numFrames) noexcept;

    bool isBypassed() const noexcept { return wet.getTargetValue() == 0.0f && ! wet.isSmoothing(); }

private:
    struct Channel
    {
        double ic1 = 0.0;
        double ic2 = 0.0;
    };

    void updateCoefficients (double cutoffHz) noexcept;
    float tick (Channel& channel, float input) const noexcept;
    void clearState() noexcept;

    const Mode mode;
    double rate = 48000.0;
    juce::SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative> cutoff;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> wet; // 0 = bypassed, 1 = filtering
    double a1 = 0.0, a2 = 0.0, a3 = 0.0;
    int coefficientCounter = 0;
    std::array<Channel, 2> channels {};
};

/** A stem's low cut then high cut, both bypassed at the default parameter values. */
class StemFilter
{
public:
    StemFilter() noexcept;

    void prepare (double sampleRate) noexcept;
    void setTargets (float lowCutHz, float highCutHz) noexcept;
    void reset() noexcept;
    void process (const float* const* in, float* const* out, int numFrames) noexcept;

private:
    CutFilter lowCut { CutFilter::Mode::highPass };
    CutFilter highCut { CutFilter::Mode::lowPass };
};
} // namespace snds::dsp
