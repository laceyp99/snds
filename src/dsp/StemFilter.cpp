#include "dsp/StemFilter.h"

#include <cmath>
#include <numbers>

namespace snds::dsp
{
namespace
{
constexpr double butterworthK = std::numbers::sqrt2; // 1 / Q for Q = 1 / sqrt2
constexpr int coefficientInterval = 8;               // samples between coefficient refreshes while sweeping
constexpr double maxCutoffOfRate = 0.49;

int parameterFor (CutFilter::Mode mode)
{
    return parameterIndex (Stem::drums, mode == CutFilter::Mode::highPass ? StemControl::lowCut : StemControl::highCut);
}
} // namespace

CutFilter::CutFilter (Mode filterMode) noexcept
    : mode (filterMode)
{
    prepare (48000.0, parameterSpec (parameterFor (mode)).smoothingSeconds);
}

void CutFilter::prepare (double sampleRate, float smoothingSeconds) noexcept
{
    rate = sampleRate;
    cutoff.reset (sampleRate, static_cast<double> (smoothingSeconds));
    wet.reset (sampleRate, static_cast<double> (smoothingSeconds));

    // Start at the default, which is the bypassed endpoint.
    const auto endpoint = static_cast<double> (parameterSpec (parameterFor (mode)).defaultValue);
    cutoff.setCurrentAndTargetValue (endpoint);
    wet.setCurrentAndTargetValue (0.0f);
    reset();
}

void CutFilter::setCutoff (float hertz) noexcept
{
    const auto clamped = clampParameterValue (parameterFor (mode), hertz);
    const auto bypassed = mode == Mode::highPass ? isLowCutBypassed (clamped) : isHighCutBypassed (clamped);

    cutoff.setTargetValue (static_cast<double> (clamped));
    wet.setTargetValue (bypassed ? 0.0f : 1.0f);
    coefficientCounter = 0; // refresh on the first sample of the next block

    if (! cutoff.isSmoothing()) // no sweep to refresh them (the target is where it already is)
        updateCoefficients (cutoff.getCurrentValue());
}

void CutFilter::clearState() noexcept
{
    channels = {};
}

void CutFilter::reset() noexcept
{
    cutoff.setCurrentAndTargetValue (cutoff.getTargetValue());
    wet.setCurrentAndTargetValue (wet.getTargetValue());
    updateCoefficients (cutoff.getCurrentValue());
    coefficientCounter = 0;
    clearState();
}

void CutFilter::updateCoefficients (double cutoffHz) noexcept
{
    const auto hz = std::min (cutoffHz, maxCutoffOfRate * rate);
    const auto g = std::tan (std::numbers::pi * hz / rate);
    a1 = 1.0 / (1.0 + g * (g + butterworthK));
    a2 = g * a1;
    a3 = g * a2;
}

float CutFilter::tick (Channel& channel, float input) const noexcept
{
    const auto x = static_cast<double> (input);
    const auto v3 = x - channel.ic2;
    const auto v1 = a1 * channel.ic1 + a2 * v3;
    const auto v2 = channel.ic2 + a2 * channel.ic1 + a3 * v3;
    channel.ic1 = 2.0 * v1 - channel.ic1;
    channel.ic2 = 2.0 * v2 - channel.ic2;

    return static_cast<float> (mode == Mode::lowPass ? v2 : x - butterworthK * v1 - v2);
}

void CutFilter::process (const float* const* in, float* const* out, int numFrames) noexcept
{
    if (isBypassed())
    {
        // Fully bypassed: an exact copy, and no state to carry (the crossfade starts from zero).
        cutoff.setCurrentAndTargetValue (cutoff.getTargetValue());
        clearState();

        for (int c = 0; c < 2; ++c)
            if (in[c] != out[c])
                std::copy_n (in[c], numFrames, out[c]);
        return;
    }

    for (int i = 0; i < numFrames; ++i)
    {
        const auto mix = wet.getNextValue();

        if (cutoff.isSmoothing())
        {
            const auto hz = cutoff.getNextValue();

            if ((coefficientCounter++ % coefficientInterval) == 0 || ! cutoff.isSmoothing())
                updateCoefficients (hz);
        }

        for (int c = 0; c < 2; ++c)
        {
            const auto x = in[c][i];
            const auto y = tick (channels[static_cast<std::size_t> (c)], x);
            out[c][i] = mix >= 1.0f ? y : x + mix * (y - x);
        }
    }

    // A non-finite input must not poison the filter forever.
    for (const auto& channel : channels)
        if (! std::isfinite (channel.ic1) || ! std::isfinite (channel.ic2))
            clearState();
}

//==============================================================================
StemFilter::StemFilter() noexcept = default;

void StemFilter::prepare (double sampleRate) noexcept
{
    lowCut.prepare (sampleRate, parameterSpec (parameterIndex (Stem::drums, StemControl::lowCut)).smoothingSeconds);
    highCut.prepare (sampleRate, parameterSpec (parameterIndex (Stem::drums, StemControl::highCut)).smoothingSeconds);
}

void StemFilter::setTargets (float lowCutHz, float highCutHz) noexcept
{
    lowCut.setCutoff (lowCutHz);
    highCut.setCutoff (highCutHz);
}

void StemFilter::reset() noexcept
{
    lowCut.reset();
    highCut.reset();
}

void StemFilter::process (const float* const* in, float* const* out, int numFrames) noexcept
{
    lowCut.process (in, out, numFrames);
    highCut.process (out, out, numFrames);
}
} // namespace snds::dsp
