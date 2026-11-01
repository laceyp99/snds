#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace snds::audio
{
/** Streaming stereo sample-rate converter: Kaiser-windowed sinc with the cutoff scaled below
    the lower of the two Nyquist frequencies, so downsampling (for example a 192 kHz song on a
    48 kHz device) is band-limited rather than aliased.

    Equal rates bypass the filter and copy frames exactly.

    Source frames are pushed in any block size and device frames are pulled in any block size.
    The result does not depend on how either side is chopped up. prepare() allocates; reset(),
    push() and pull() do not, so they are safe on the audio thread.

    Time: the converter reports `getNextOutputTime()`, the source-frame time (since reset) of
    the next frame pull() will produce. This is the playhead the transport clock reads. Because
    the filter looks `getLookaheadFrames()` frames ahead, the source side must be pushed that far
    past the playhead; at the end of a song push that many zeros to flush the tail.
*/
class Resampler
{
public:
    static constexpr int channelCount = 2;

    /** `maxPushFrames` bounds the size of one push(). */
    void prepare (double sourceRate, double destinationRate, int maxPushFrames);

    /** Forgets all history; the next output is at source time 0. */
    void reset();

    bool isBypass() const noexcept { return bypass; }
    int getLookaheadFrames() const noexcept { return halfWidth; }
    double getRatio() const noexcept { return ratio; }

    void push (const float* const* source, int numFrames) noexcept;

    /** Produces up to `maxFrames`, as many as the pushed input allows. Returns how many. */
    int pull (float* const* destination, int maxFrames) noexcept;

    double getNextOutputTime() const noexcept { return bypass ? bypassTime : position + discarded - halfWidth; }

private:
    void compact() noexcept;

    bool bypass = true;
    double ratio = 1.0;  // source frames per destination frame
    double cutoff = 1.0; // filter scale (<= 1)
    int halfWidth = 0;   // M: taps reach M-1 behind and M ahead of the output instant
    int maxPush = 0;

    std::vector<float> table;
    std::vector<float> weights;
    std::vector<float> buffer[channelCount];
    int filled = 0;          // valid frames in the buffer
    double position = 0.0;   // next output instant, in buffer indices (non-bypass)
    double bypassTime = 0.0; // frames passed so far (bypass)
    double discarded = 0.0;  // buffer frames dropped by compaction (non-bypass)
};
} // namespace snds::audio
