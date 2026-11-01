#pragma once

#include "dsp/ParameterStore.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>

namespace snds::dsp
{
/** Which stems are audible, given each stem's mute and solo state.

    Solo is additive: with no solo active every unmuted stem plays; with any solo active only
    soloed stems play. Mute always wins: a stem that is both muted and soloed is silent (and its
    solo still counts as "a solo is active", so the other unsoloed stems stay silent too).
*/
constexpr std::array<bool, numStems> stemAudibility (const std::array<bool, numStems>& mute,
                                                     const std::array<bool, numStems>& solo) noexcept
{
    bool anySolo = false;
    for (const auto s : solo)
        anySolo = anySolo || s;

    std::array<bool, numStems> audible {};
    for (std::size_t i = 0; i < audible.size(); ++i)
        audible[i] = ! mute[i] && (! anySolo || solo[i]);
    return audible;
}

/** The same decision read from a parameter snapshot. */
std::array<bool, numStems> stemAudibility (const ParameterSnapshot& snapshot) noexcept;

/** One stem's fader, mute/solo gate and pan, applied to a stereo signal.

    Everything is smoothed in the linear domain so changes are click-free: the fader gain and
    the audibility gate (mute/solo) ramp separately, and pan ramps in the pan domain with the
    gains recomputed per sample while it moves. The fader reaches +6 dB; at -60 dB (-inf) the
    stem is exactly silent.

    Pan is a constant-power law normalised to unity at the centre: gainL = sqrt2 cos(theta),
    gainR = sqrt2 sin(theta), theta = (pan + 1) pi/4. The centre passes the stem through
    untouched; at the extremes the favoured channel is +3 dB and the other is silent (the stem's
    power across both channels is constant for any pan position).

    Audio thread: prepare/setTargets/process/reset do not allocate or lock. Anything may read
    getPanPosition from another thread.
*/
class StemLevelStage
{
public:
    StemLevelStage() noexcept;

    /** Sets the ramp lengths from the parameter smoothing times and snaps to the current targets. */
    void prepare (double sampleRate) noexcept;

    /** Targets from the parameters; the audio ramps towards them. Values are clamped. */
    void setTargets (float gainDb, bool audible, float pan) noexcept;

    /** Jumps to the current targets without a ramp (for a new song, never mid-playback). */
    void snapToTargets() noexcept;

    /** Stereo in -> stereo out. In-place (in == out) is fine. */
    void process (const float* const* in, float* const* out, int numFrames) noexcept;

    /** The smoothed pan (-1 left .. +1 right) as of the end of the last block; always in range. */
    float getPanPosition() const noexcept { return panPosition.load (std::memory_order_relaxed); }

    /** The gains pan applies at a position (clamped to -1..+1). Exactly 1/1 at the centre. */
    static std::array<float, 2> panGains (float pan) noexcept;

private:
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> fader;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gate;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> pan;
    std::atomic<float> panPosition { 0.0f };
};
} // namespace snds::dsp
