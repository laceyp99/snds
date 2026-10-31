#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace snds::dsp
{
/** The single reverb all stems share, fed by the reverb send bus.

    A fixed-character Freeverb (juce::Reverb) runs fully wet; the only live control is the
    global return level (0..1, smoothed). Its input is the stereo send bus (mixed to mono by the
    reverb itself) and its output is the stereo wet signal to add to the master sum.

    Audio thread: prepare allocates; everything else is allocation- and lock-free.
*/
class SharedReverb
{
public:
    SharedReverb();

    void prepare (double sampleRate);

    void setReturn (float gain) noexcept;

    /** Clears the tail and snaps the return level to its target. */
    void reset() noexcept;

    /** send -> wetOut (stereo each, distinct buffers). wetOut = return * reverb(send). */
    void process (const float* const* send, float* const* wetOut, int numFrames) noexcept;

private:
    juce::Reverb reverb;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> returnGain;
};
} // namespace snds::dsp
