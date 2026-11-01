#pragma once

#include "dsp/MasterStage.h"
#include "dsp/MixMeters.h"
#include "dsp/ParameterStore.h"
#include "dsp/SharedReverb.h"
#include "dsp/StemFilter.h"
#include "dsp/StemLevelStage.h"
#include "dsp/StemSends.h"
#include "dsp/TempoDelay.h"
#include "dsp/VisualAnalysis.h"

#include <array>
#include <vector>

namespace snds::dsp
{
/** The interactive mix: four stereo stems in, one limited stereo mix out, at the source rate.

    Signal flow per block:

        stem i -> low cut -> high cut -> fader/mute/solo gate -> pan ---+--> (tap + meter)
                                                                        |
                                    reverb send_i, delay send_i (post-pan, post-fader)
                                                                        |
        sum of the four stems  +  shared reverb return  +  tempo delay return
            -> master gain -> safety limiter (-0.3 dBFS) -> output

    The mix is summed in float with no internal scaling, in the order ((s0 + s1) + s2) + s3, so
    the default state (every control at its default, effects silent) reproduces the plain sum
    bit for bit as long as the limiter is idle.

    Threads. The message thread owns the ParameterStore it passed in and writes controls into it
    at any time; the audio thread reads one consistent snapshot per process() call and every
    audible parameter ramps to it. meters() may be read from any thread. prepare() allocates and
    must not run concurrently with process(); everything else on the audio path is allocation-,
    lock- and I/O-free.

    The three audio-facing methods match audio::StemMixStage exactly:
    prepare(double, int), reset() noexcept, process(stems, output, numFrames) noexcept.
*/
class MixProcessor
{
public:
    /** `parameters` must outlive the processor. */
    explicit MixProcessor (ParameterStore& parameters);

    /** Not real-time safe. Allocates everything and resets. */
    void prepare (double sourceRate, int maxBlockFrames);

    /** After a seek, loop wrap or load: clears delay, reverb and filter state, snaps every
        smoother and the limiter to the current parameters, zeroes the meters. */
    void reset() noexcept;

    /** `stems[i][channel][frame]` for drums, bass, other, vocals; `output[channel][frame]`
        receives the mix. Any numFrames is accepted (long blocks are processed in chunks of the
        prepared size). Input and output may not overlap. */
    void process (const std::array<const float* const*, numStems>& stems, float* const* output, int numFrames) noexcept;

    //==========================================================================
    // Visuals and metering.

    /** Per-stem post-fader peak/RMS, master peak and limiter activity. Any thread. */
    const MeterStore& meters() const noexcept { return meterStore; }

    /** Bounded, smoothed drums pulse, bass depth, vocal glow, other texture, pan and the visual clock,
        analysed from the post-pan stem signals. Any thread (the renderer reads this). */
    const VisualStore& visuals() const noexcept { return visualAnalyzer.store(); }

    /** The smoothed pan position (-1..+1) of a stem. Any thread. */
    float getPanPosition (int stem) const noexcept
    {
        return levelStages[static_cast<std::size_t> (stem)].getPanPosition();
    }

    /** The four post-filter, post-fader, post-pan stereo signals of the most recent chunk (the
        signals that feed the meters, sends and sum). Valid on the audio thread, from the end of
        process() until the next call: the hook for per-stem analysis. */
    std::array<const float* const*, numStems> lastStemSignals() const noexcept;
    int lastFrameCount() const noexcept { return lastFrames; }

    double getSampleRate() const noexcept { return rate; }
    int getMaxBlockFrames() const noexcept { return maxFrames; }

private:
    void applyParameters() noexcept;
    void processChunk (const std::array<const float* const*, numStems>& stems,
                       float* const* output,
                       int offset,
                       int numFrames) noexcept;

    ParameterStore& parameters;
    ParameterSnapshot snapshot;

    double rate = 48000.0;
    int maxFrames = 0;
    int lastFrames = 0;

    std::array<StemFilter, numStems> filters;
    std::array<StemLevelStage, numStems> levelStages;
    StemSends sends;
    SharedReverb reverb;
    TempoDelay delay;
    MasterStage master;

    MeterStore meterStore;
    VisualAnalyzer visualAnalyzer;
    std::array<LevelFollower, numStems> stemMeters;
    LevelFollower masterMeter;
    LevelFollower limiterInputMeter;
    LevelFollower limiterReductionMeter;

    // Twelve channels: four stereo stems, then the reverb and delay returns.
    std::vector<float> storage;
    std::array<std::array<float*, 2>, numStems> stemChannels {};
    std::array<float*, 2> reverbReturn {};
    std::array<float*, 2> delayReturn {};
};
} // namespace snds::dsp
