#pragma once

#include "dsp/ParameterStore.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <vector>

namespace snds::dsp
{
/** A fixed pad (-14 dB) on every send, so a send of 1 is one fifth of that stem's level on the bus.
    Four stems at full send then sum to at most 0.8 of one stem's level, which keeps the reverb and
    delay from adding much more than ~2 dB to the peak of a full mix (measured with four full-scale
    noise stems, all sends and returns at maximum). */
inline constexpr float sendBusPad = 0.2f;

/** The per-stem reverb and delay sends, summed into two stereo buses.

    Each stem feeds the buses from its post-filter, post-fader, post-pan signal, so muting a stem
    (or soloing others) also silences its sends, and the fader scales how much it sends. Send
    gains (0..1, linear, times sendBusPad) are smoothed per stem and per bus. A send that is at zero and not moving
    costs nothing.

    Audio thread: nothing here allocates after prepare. Blocks must not exceed the prepared size.
*/
class StemSends
{
public:
    void prepare (double sampleRate, int maxBlockFrames);

    /** Reads the eight send values from the parameters; the gains ramp to them. */
    void setTargets (const ParameterSnapshot& snapshot) noexcept;

    /** Snaps the gains to their targets (nothing in the buses to clear). */
    void reset() noexcept;

    /** Zeroes the buses for a block of this many frames. */
    void beginBlock (int numFrames) noexcept;

    /** Adds one stem's stereo signal to both buses at its send gains. */
    void addStem (int stem, const float* const* signal, int numFrames) noexcept;

    float* const* reverbBus() noexcept { return reverbPointers.data(); }
    float* const* delayBus() noexcept { return delayPointers.data(); }

private:
    using Smoother = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;

    static void mixInto (Smoother& gain, const float* const* signal, float* const* bus, int numFrames) noexcept;

    int maxFrames = 0;
    std::array<Smoother, numStems> reverbGain;
    std::array<Smoother, numStems> delayGain;
    std::vector<float> storage;
    std::array<float*, 2> reverbPointers {};
    std::array<float*, 2> delayPointers {};
};
} // namespace snds::dsp
