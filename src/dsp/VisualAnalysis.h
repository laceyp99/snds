#pragma once

#include "dsp/MixParameters.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace snds::dsp
{
/** What the visuals know about the mix: bounded, smoothed, post-fader values for the four stem
    roles, published by the audio thread through VisualStore.

    Every unit value is in [0, 1] and is exactly 0 for a silent stem (a muted, soloed-out or
    faded-out stem contributes nothing). The values come from the post-filter, post-fader,
    post-pan stem signals, so they follow what the user hears. The mapping is deterministic: the
    same stem input and the same block partition always give the same snapshot sequence.

    Stem roles (the "reactive mapping contract"):
      - drums: a rhythmic pulse (onset driven, not level driven)
      - bass: depth (the low band's level)
      - vocals: glow (the level of the whole stem)
      - other: texture (the high band's level)
      - pan: each stem's smoothed pan position, with its level to weight it
*/
struct VisualSnapshot
{
    float drumsPulse = 0.0f;   // 0..1, jumps on a drum onset and decays (about 120 ms)
    float bassDepth = 0.0f;    // 0..1, low-band (below ~120 Hz) level of the bass stem
    float vocalGlow = 0.0f;    // 0..1, level of the vocal stem
    float otherTexture = 0.0f; // 0..1, high-band (above ~2 kHz) level of the other stem

    std::array<float, numStems> pan {};   // -1 (left) .. +1 (right); as set, even when the stem is silent
    std::array<float, numStems> level {}; // 0..1, post-fader level of each whole stem (weights the pan)

    // The visual clock. It does not depend on the engine or the song position.
    float bpm = tempoDefaultBpm;      // the tempo control's value
    float beatPhase = 0.0f;           // 0..1, free-running at `bpm` since the last reset
    float sampleRate = 48000.0f;      // of the analysed audio
    std::uint64_t framesAnalysed = 0; // monotonic; never goes backwards, not even across reset
    std::uint32_t resetCount = 0;     // how many times the analysis was restarted (seek, loop, load)
    std::uint32_t version = 0;        // number of publishes behind this snapshot

    /** Analysed audio time in seconds (monotonic). */
    double seconds() const noexcept
    {
        return sampleRate > 0.0f ? static_cast<double> (framesAnalysed) / sampleRate : 0.0;
    }
};

/** Lock-free hand-off of a VisualSnapshot from the audio thread to the renderer.

    One writer (the audio thread, wait-free) publishes whole snapshots through a seqlock of
    relaxed atomics; any number of readers get a consistent snapshot or are told to try again.
*/
class VisualStore
{
public:
    VisualStore() noexcept;

    /** Audio thread only. */
    void publish (const VisualSnapshot& snapshot) noexcept;

    /** Any thread. Returns false (leaving `out` untouched) only if a write kept colliding. */
    bool read (VisualSnapshot& out) const noexcept;

private:
    static constexpr std::size_t floatCount = 4 + 2 * numStems + 3;

    std::array<std::atomic<float>, floatCount> floats;
    std::atomic<std::uint64_t> frames { 0 };
    std::atomic<std::uint32_t> resets { 0 };
    std::atomic<std::uint32_t> sequence { 0 };
};

/** Turns the four post-pan stem signals into a VisualSnapshot, block by block.

    All filters and envelopes run per sample, so the state does not depend on how the audio is cut
    into blocks; a snapshot is published at the end of each block. Nothing allocates, ever.

    Audio thread only (process/reset). The store may be read from any thread.
*/
class VisualAnalyzer
{
public:
    VisualAnalyzer() noexcept;

    /** Sets the time constants for a sample rate and restarts the analysis. Does not allocate. */
    void prepare (double sampleRate) noexcept;

    /** A seek, loop wrap or load: envelopes and the beat phase restart, the clock keeps counting. */
    void reset() noexcept;

    /** `stems[i][channel][frame]`: drums, bass, other, vocals, post-pan. `pan` is each stem's
        smoothed pan position (-1..+1) and `bpm` the tempo control. */
    void process (const std::array<const float* const*, numStems>& stems,
                  const std::array<float, numStems>& pan,
                  float bpm,
                  int numFrames) noexcept;

    const VisualStore& store() const noexcept { return visualStore; }

    /** The most recently published snapshot, as the audio thread has it (no atomics involved). */
    const VisualSnapshot& current() const noexcept { return latest; }

private:
    struct OnePole
    {
        float state = 0.0f;
        float lowPass (float x, float coefficient) noexcept
        {
            state += coefficient * (x - state);
            return state;
        }
    };

    struct StemState
    {
        float level = 0.0f; // whole-stem envelope
        OnePole low1, low2; // the bass band
        float lowEnv = 0.0f;
        OnePole high1, high2; // the other-stem band (low-passed, then subtracted)
        float highEnv = 0.0f;
        float fast = 0.0f, slow = 0.0f; // the drum onset detector's two envelopes
        float pulse = 0.0f;
    };

    void clearState() noexcept;

    double rate = 48000.0;
    float lowCoefficient = 0.0f;
    float highCoefficient = 0.0f;
    float levelAttack = 0.0f, levelRelease = 0.0f;
    float bandAttack = 0.0f, bandRelease = 0.0f;
    float glowAttack = 0.0f, glowRelease = 0.0f;
    float fastCoefficient = 0.0f, slowCoefficient = 0.0f;
    float pulseDecay = 0.0f;

    std::array<StemState, numStems> stems {};
    double phase = 0.0;
    std::uint64_t frames = 0;
    std::uint32_t resetCount = 0;
    std::uint32_t publishes = 0;

    VisualSnapshot latest;
    VisualStore visualStore;
};
} // namespace snds::dsp
