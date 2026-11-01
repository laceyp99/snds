#pragma once

#include "dsp/MixParameters.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <vector>

namespace snds::dsp
{
/** A stereo feedback delay whose time follows the BPM and a musical division.

    Each channel has its own delay line and feeds back into itself (a plain stereo delay, not
    ping-pong). The input is the stereo delay send bus; the output is the wet signal scaled by the
    global return level, to be added to the master sum.

    Time changes (BPM, division) never jump: the delay keeps two read taps on the same buffer and
    crossfades from the old delay to the new one over 50 ms with an equal-power law. If the time
    changes again mid-crossfade, the newest target waits for the crossfade to finish, so a dragged
    BPM control follows in glitch-free hops. Delays are whole samples (no interpolation, so no
    pitch artefacts); the buffer is sized in prepare for the slowest BPM and longest division.

    The feedback path is tamed: a 30 Hz DC blocker and a 7 kHz one-pole low-pass sit in the loop
    (the first echo is clean, repeats get darker) and the feedback gain is clamped to 0.6 (maxFeedback),
    so the loop is always stable. Worst case, a tone at the delay's resonance reaches at most
    1 / (1 - feedback) = 2.5x its level; the master limiter is the final guard.

    Samples entering the loop are sanitised, so one NaN cannot recirculate forever.

    Audio thread: prepare allocates; everything else is allocation- and lock-free.
*/
class TempoDelay
{
public:
    TempoDelay();

    /** Allocates the delay lines (longest delay: maxDelayTimeSeconds()). Not real-time safe. */
    void prepare (double sampleRate);

    void setTempo (double bpm, DelayDivision division) noexcept;
    void setFeedback (float feedback) noexcept;
    void setReturn (float gain) noexcept;

    /** Clears the lines and the loop filters; snaps time, feedback and return to their targets. */
    void reset() noexcept;

    /** send -> wetOut (stereo each, distinct buffers). */
    void process (const float* const* send, float* const* wetOut, int numFrames) noexcept;

    /** The delay (in samples) the output is settling on, and the longest the buffer supports. */
    int getTargetDelaySamples() const noexcept;
    int getMaxDelaySamples() const noexcept { return maxDelay; }
    bool isCrossfading() const noexcept { return crossfading; }
    int getCrossfadeSamples() const noexcept { return crossfadeLength; }

private:
    struct LoopFilter
    {
        double hpIn = 0.0, hpOut = 0.0, lp = 0.0;
    };

    int delaySamplesFor (double bpm, DelayDivision division) const noexcept;
    void startCrossfade (int newDelay) noexcept;

    double rate = 48000.0;
    int maxDelay = 1;
    int size = 4;
    int writePosition = 0;

    int delayA = 1; // the settled delay
    int delayB = 1; // the delay being faded in
    int pendingDelay = -1;
    bool crossfading = false;
    int crossfadePosition = 0;
    int crossfadeLength = 1;

    double lowPassCoefficient = 0.5;
    double dcBlockCoefficient = 0.99;

    std::array<std::vector<float>, 2> lines;
    std::array<LoopFilter, 2> filters {};

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> feedback;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> returnGain;
};
} // namespace snds::dsp
