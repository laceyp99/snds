#pragma once

#include "dsp/MixParameters.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace snds::dsp
{
/** Meter ballistics, shared by every meter in the mix.

    Peak: instant attack, exponential release with a 300 ms time constant (about -29 dB per second).
    RMS: a one-pole average of the mean square over both channels, 300 ms time constant.
    Both are advanced once per processed block and scaled by the block length, so the readings
    do not depend on the block size (a peak is only seen at block resolution).
*/
inline constexpr double meterPeakReleaseSeconds = 0.3;
inline constexpr double meterRmsSeconds = 0.3;

/** Gain reduction at or above this (dB) counts as the limiter being active. */
inline constexpr float limiterActivityThresholdDb = 0.1f;

struct StemMeter
{
    float peak = 0.0f; // post-filter, post-fader, post-pan; linear
    float rms = 0.0f;
};

struct MeterSnapshot
{
    std::array<StemMeter, numStems> stems {};

    float masterPeak = 0.0f; // after the limiter
    float masterRms = 0.0f;
    float limiterInputPeak = 0.0f;   // before the limiter (after master gain); above the ceiling means it worked
    float limiterReductionDb = 0.0f; // >= 0, held with the same release as the peak meters

    bool limiterActive() const noexcept { return limiterReductionDb >= limiterActivityThresholdDb; }

    std::uint32_t version = 0; // number of publishes behind this snapshot
};

/** Follows the level of a stereo signal block by block (audio thread). */
class LevelFollower
{
public:
    void prepare (double sampleRate) noexcept
    {
        rate = sampleRate;
        reset();
    }

    void reset() noexcept
    {
        peakValue = 0.0f;
        meanSquare = 0.0f;
    }

    /** One block of a stereo signal. */
    void update (const float* const* channels, int numFrames) noexcept;

    /** A block whose peak is already known (and which has no RMS), e.g. a limiter's input peak. */
    void updatePeak (float blockPeak, int numFrames) noexcept;

    float peak() const noexcept { return peakValue; }
    float rms() const noexcept;

private:
    double rate = 48000.0;
    float peakValue = 0.0f;
    float meanSquare = 0.0f;
};

/** Converts a linear gain (<= 1) to a positive reduction in decibels. */
float reductionToDecibels (float gain) noexcept;

/** Lock-free hand-off of a MeterSnapshot from the audio thread to the UI.

    One writer (the audio thread, wait-free) publishes whole snapshots through a seqlock of
    relaxed atomics; any number of readers get a consistent snapshot or are told to try again.
*/
class MeterStore
{
public:
    MeterStore() noexcept;

    /** Audio thread only. */
    void publish (const MeterSnapshot& snapshot) noexcept;

    /** Any thread. Returns false (leaving `out` untouched) only if a write kept colliding. */
    bool read (MeterSnapshot& out) const noexcept;

private:
    static constexpr std::size_t fieldCount = numStems * 2 + 4;

    std::array<std::atomic<float>, fieldCount> fields;
    std::atomic<std::uint32_t> sequence { 0 };
};
} // namespace snds::dsp
