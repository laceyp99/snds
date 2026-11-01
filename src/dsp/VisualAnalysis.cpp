#include "dsp/VisualAnalysis.h"

#include "dsp/Sanitize.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace snds::dsp
{
namespace
{
constexpr int maxReadAttempts = 8;
constexpr float flushBelow = 1.0e-9f;
constexpr float pulseFlush = 1.0e-4f; // a pulse this small is gone (keeps a silent stem at exactly 0)

// Analysis bands and time constants (seconds / Hz).
constexpr double bassCornerHz = 120.0;
constexpr double textureCornerHz = 2000.0;
constexpr double levelAttackSeconds = 0.010;
constexpr double levelReleaseSeconds = 0.200;
constexpr double bandAttackSeconds = 0.015;
constexpr double bandReleaseSeconds = 0.200;
constexpr double glowAttackSeconds = 0.025;
constexpr double glowReleaseSeconds = 0.300;
// The onset detector compares two symmetric averages of |x|: in steady sound they agree, so only a
// rise (a hit) makes the fast one lead the slow one.
constexpr double fastSeconds = 0.004;
constexpr double slowSeconds = 0.060;
constexpr double pulseDecaySeconds = 0.120;

// onset = fast - ratio * slow - floor. The floor ignores noise, the ratio ignores ripple on steady
// sound; the scale makes a hit at about -12 dBFS read ~0.75.
constexpr float onsetRatio = 1.5f;
constexpr float onsetFloor = 0.005f;
constexpr float onsetScale = 0.06f;

// Envelope amplitude to 0..1: -48 dBFS and below is 0, -6 dBFS and above is 1.
constexpr float unitFloorDb = -48.0f;
constexpr float unitCeilingDb = -6.0f;

float unitFromAmplitude (float amplitude) noexcept
{
    const auto db = 20.0f * std::log10 (std::max (amplitude, 1.0e-9f));
    return std::clamp ((db - unitFloorDb) / (unitCeilingDb - unitFloorDb), 0.0f, 1.0f);
}

float coefficientFor (double seconds, double rate) noexcept
{
    return static_cast<float> (1.0 - std::exp (-1.0 / (seconds * rate)));
}

float follow (float envelope, float input, float attack, float release) noexcept
{
    return envelope + (input - envelope) * (input > envelope ? attack : release);
}

float finiteOr (float value, float fallback) noexcept
{
    return std::isfinite (value) ? value : fallback;
}
} // namespace

//==============================================================================
VisualStore::VisualStore() noexcept
{
    for (auto& f : floats)
        f.store (0.0f, std::memory_order_relaxed);
}

void VisualStore::publish (const VisualSnapshot& s) noexcept
{
    const auto start = sequence.load (std::memory_order_relaxed) + 1; // odd: write in progress
    sequence.store (start, std::memory_order_relaxed);
    std::atomic_thread_fence (std::memory_order_release);

    std::size_t i = 0;
    floats[i++].store (s.drumsPulse, std::memory_order_relaxed);
    floats[i++].store (s.bassDepth, std::memory_order_relaxed);
    floats[i++].store (s.vocalGlow, std::memory_order_relaxed);
    floats[i++].store (s.otherTexture, std::memory_order_relaxed);
    for (const auto v : s.pan)
        floats[i++].store (v, std::memory_order_relaxed);
    for (const auto v : s.level)
        floats[i++].store (v, std::memory_order_relaxed);
    floats[i++].store (s.bpm, std::memory_order_relaxed);
    floats[i++].store (s.beatPhase, std::memory_order_relaxed);
    floats[i++].store (s.sampleRate, std::memory_order_relaxed);
    frames.store (s.framesAnalysed, std::memory_order_relaxed);
    resets.store (s.resetCount, std::memory_order_relaxed);

    sequence.store (start + 1, std::memory_order_release);
}

bool VisualStore::read (VisualSnapshot& out) const noexcept
{
    for (int attempt = 0; attempt < maxReadAttempts; ++attempt)
    {
        const auto before = sequence.load (std::memory_order_acquire);

        if ((before & 1u) != 0)
            continue;

        std::array<float, floatCount> copy;
        for (std::size_t i = 0; i < floatCount; ++i)
            copy[i] = floats[i].load (std::memory_order_relaxed);
        const auto framesCopy = frames.load (std::memory_order_relaxed);
        const auto resetsCopy = resets.load (std::memory_order_relaxed);

        std::atomic_thread_fence (std::memory_order_acquire);

        if (sequence.load (std::memory_order_relaxed) != before)
            continue;

        std::size_t i = 0;
        out.drumsPulse = copy[i++];
        out.bassDepth = copy[i++];
        out.vocalGlow = copy[i++];
        out.otherTexture = copy[i++];
        for (auto& v : out.pan)
            v = copy[i++];
        for (auto& v : out.level)
            v = copy[i++];
        out.bpm = copy[i++];
        out.beatPhase = copy[i++];
        out.sampleRate = copy[i++];
        out.framesAnalysed = framesCopy;
        out.resetCount = resetsCopy;
        out.version = before >> 1;
        return true;
    }

    return false;
}

//==============================================================================
VisualAnalyzer::VisualAnalyzer() noexcept
{
    prepare (48000.0);
}

void VisualAnalyzer::prepare (double sampleRate) noexcept
{
    rate = sampleRate;
    lowCoefficient = coefficientFor (1.0 / (2.0 * std::numbers::pi * bassCornerHz), rate);
    highCoefficient = coefficientFor (1.0 / (2.0 * std::numbers::pi * textureCornerHz), rate);
    levelAttack = coefficientFor (levelAttackSeconds, rate);
    levelRelease = coefficientFor (levelReleaseSeconds, rate);
    bandAttack = coefficientFor (bandAttackSeconds, rate);
    bandRelease = coefficientFor (bandReleaseSeconds, rate);
    glowAttack = coefficientFor (glowAttackSeconds, rate);
    glowRelease = coefficientFor (glowReleaseSeconds, rate);
    fastCoefficient = coefficientFor (fastSeconds, rate);
    slowCoefficient = coefficientFor (slowSeconds, rate);
    pulseDecay = static_cast<float> (std::exp (-1.0 / (pulseDecaySeconds * rate)));

    clearState();
    phase = 0.0;
    frames = 0;
    resetCount = 0;
    publishes = 0;
    latest = VisualSnapshot();
    latest.sampleRate = static_cast<float> (rate);
    visualStore.publish (latest);
}

void VisualAnalyzer::clearState() noexcept
{
    stems = {};
}

void VisualAnalyzer::reset() noexcept
{
    clearState();
    phase = 0.0;
    ++resetCount;

    // Silence, but the clock keeps its place: the renderer sees time keep moving forward.
    const auto pan = latest.pan;
    const auto bpm = latest.bpm;
    latest = VisualSnapshot();
    latest.pan = pan;
    latest.bpm = bpm;
    latest.sampleRate = static_cast<float> (rate);
    latest.framesAnalysed = frames;
    latest.resetCount = resetCount;
    latest.version = ++publishes;
    visualStore.publish (latest);
}

void VisualAnalyzer::process (const std::array<const float* const*, numStems>& inputs,
                              const std::array<float, numStems>& pan,
                              float bpm,
                              int numFrames) noexcept
{
    for (std::size_t s = 0; s < stems.size(); ++s)
    {
        auto& st = stems[s];
        const float* left = inputs[s][0];
        const float* right = inputs[s][1];

        for (int i = 0; i < numFrames; ++i)
        {
            const auto x = sanitiseSample (0.5f * (left[i] + right[i]));
            const auto a = std::abs (x);

            switch (s)
            {
                case 0: // drums: onset = how far the fast envelope is above the slow one
                {
                    st.level = follow (st.level, a, levelAttack, levelRelease);
                    st.fast += (a - st.fast) * fastCoefficient;
                    st.slow += (a - st.slow) * slowCoefficient;
                    const auto onset = std::max (0.0f, st.fast - onsetRatio * st.slow - onsetFloor);
                    st.pulse = std::max (st.pulse * pulseDecay, onset / (onset + onsetScale));
                    break;
                }
                case 1: // bass: the low band
                {
                    st.level = follow (st.level, a, levelAttack, levelRelease);
                    const auto low = st.low2.lowPass (st.low1.lowPass (x, lowCoefficient), lowCoefficient);
                    st.lowEnv = follow (st.lowEnv, std::abs (low), bandAttack, bandRelease);
                    break;
                }
                case 2: // other: the high band (two one-pole high-passes in series)
                {
                    st.level = follow (st.level, a, levelAttack, levelRelease);
                    const auto first = x - st.high1.lowPass (x, highCoefficient);
                    const auto high = first - st.high2.lowPass (first, highCoefficient);
                    st.highEnv = follow (st.highEnv, std::abs (high), bandAttack, bandRelease);
                    break;
                }
                default: // vocals: a slow envelope of the whole stem
                    st.level = follow (st.level, a, levelAttack, levelRelease);
                    st.lowEnv = follow (st.lowEnv, a, glowAttack, glowRelease);
                    break;
            }
        }

        if (st.pulse < pulseFlush)
            st.pulse = 0.0f;

        for (auto* v : { &st.level,
                         &st.lowEnv,
                         &st.highEnv,
                         &st.fast,
                         &st.slow,
                         &st.pulse,
                         &st.low1.state,
                         &st.low2.state,
                         &st.high1.state,
                         &st.high2.state })
            if (std::abs (*v) < flushBelow)
                *v = 0.0f;
    }

    frames += static_cast<std::uint64_t> (numFrames);

    const auto safeBpm = std::clamp (finiteOr (bpm, tempoDefaultBpm), tempoMinBpm, tempoMaxBpm);
    phase += static_cast<double> (safeBpm) / (60.0 * rate) * numFrames;
    phase -= std::floor (phase);

    latest.drumsPulse = std::clamp (stems[0].pulse, 0.0f, 1.0f);
    latest.bassDepth = unitFromAmplitude (stems[1].lowEnv);
    latest.otherTexture = unitFromAmplitude (stems[2].highEnv);
    latest.vocalGlow = unitFromAmplitude (stems[3].lowEnv);

    for (std::size_t s = 0; s < stems.size(); ++s)
    {
        latest.pan[s] = std::clamp (finiteOr (pan[s], 0.0f), panMin, panMax);
        latest.level[s] = unitFromAmplitude (stems[s].level);
    }

    latest.bpm = safeBpm;
    latest.beatPhase = std::min (static_cast<float> (phase), 0.99999994f); // never rounds up to 1
    latest.sampleRate = static_cast<float> (rate);
    latest.framesAnalysed = frames;
    latest.resetCount = resetCount;
    latest.version = ++publishes;
    visualStore.publish (latest);
}
} // namespace snds::dsp
