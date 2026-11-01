#pragma once

#include <array>

namespace snds::audio
{
/** The four stems in cache order: drums, bass, other, vocals. */
constexpr int stemChannelCount = 2;
constexpr int stemCount = 4;

/** Where the four aligned stems become one stereo mix, at the SOURCE sample rate, before the
    A/B crossfade and the device-rate conversion.

    This is the seam the interactive mix engine plugs into (per-stem gain, mute/solo, filters,
    shared effects, limiter, meters), and where post-fader stem levels can be tapped for the
    visuals. process() runs on the audio thread: no allocation, locks, I/O or logging.

    The engine calls process() with the same `numFrames` of every stem, `numFrames` at most
    the `maxBlockFrames` given to prepare(), in order and without gaps, except after reset().
*/
class StemMixStage
{
public:
    virtual ~StemMixStage() = default;

    /** Message thread (or any non-audio thread), before streaming starts. */
    virtual void prepare (double sourceRate, int maxBlockFrames) = 0;

    /** Clears per-stream state (delay lines, filters, smoothers); called after a seek, a loop
        wrap or a reload. Audio thread; must not allocate. */
    virtual void reset() noexcept = 0;

    /** `stems[i][channel][frame]` for drums, bass, other, vocals; `output[channel][frame]`
        receives the mix (both channels, `numFrames` frames). */
    virtual void process (const std::array<const float* const*, stemCount>& stems,
                          float* const* output,
                          int numFrames) noexcept = 0;
};

/** The Phase 3 mix: every stem at unity gain, summed in stem order. */
class UnityStemMixStage final : public StemMixStage
{
public:
    void prepare (double, int) override {}
    void reset() noexcept override {}

    void process (const std::array<const float* const*, stemCount>& stems,
                  float* const* output,
                  int numFrames) noexcept override
    {
        for (int channel = 0; channel < stemChannelCount; ++channel)
        {
            const float* drums = stems[0][channel];
            const float* bass = stems[1][channel];
            const float* other = stems[2][channel];
            const float* vocals = stems[3][channel];
            float* destination = output[channel];

            for (int i = 0; i < numFrames; ++i)
                destination[i] = ((drums[i] + bass[i]) + other[i]) + vocals[i];
        }
    }
};
} // namespace snds::audio
