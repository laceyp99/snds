#include "dsp/TempoDelay.h"

#include "dsp/Sanitize.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace snds::dsp
{
namespace
{
constexpr double crossfadeSeconds = 0.05;
constexpr double loopLowPassHz = 7000.0;
constexpr double loopDcBlockHz = 30.0;
} // namespace

TempoDelay::TempoDelay()
{
    prepare (48000.0);
}

void TempoDelay::prepare (double sampleRate)
{
    rate = sampleRate;
    maxDelay = static_cast<int> (std::ceil (maxDelayTimeSeconds() * sampleRate)) + 1;
    size = maxDelay + 2;

    for (auto& line : lines)
        line.assign (static_cast<std::size_t> (size), 0.0f);

    crossfadeLength = std::max (1, static_cast<int> (std::lround (crossfadeSeconds * sampleRate)));
    lowPassCoefficient = 1.0 - std::exp (-2.0 * std::numbers::pi * loopLowPassHz / sampleRate);
    dcBlockCoefficient = std::exp (-2.0 * std::numbers::pi * loopDcBlockHz / sampleRate);

    feedback.reset (
        sampleRate,
        static_cast<double> (parameterSpec (parameterIndex (GlobalControl::delayFeedback)).smoothingSeconds));
    returnGain.reset (
        sampleRate, static_cast<double> (parameterSpec (parameterIndex (GlobalControl::delayReturn)).smoothingSeconds));
    feedback.setCurrentAndTargetValue (parameterSpec (parameterIndex (GlobalControl::delayFeedback)).defaultValue);
    returnGain.setCurrentAndTargetValue (parameterSpec (parameterIndex (GlobalControl::delayReturn)).defaultValue);

    delayA = delayB =
        delaySamplesFor (static_cast<double> (tempoDefaultBpm),
                         toDelayDivision (parameterSpec (parameterIndex (GlobalControl::delayDivision)).defaultValue));
    pendingDelay = -1;
    crossfading = false;
    crossfadePosition = 0;
    writePosition = 0;
    filters = {};
}

int TempoDelay::delaySamplesFor (double bpm, DelayDivision division) const noexcept
{
    const auto clampedBpm = std::clamp (bpm, static_cast<double> (tempoMinBpm), static_cast<double> (tempoMaxBpm));
    const auto samples = static_cast<int> (std::lround (delayTimeSeconds (clampedBpm, division) * rate));
    return std::clamp (samples, 1, maxDelay);
}

int TempoDelay::getTargetDelaySamples() const noexcept
{
    if (pendingDelay >= 0)
        return pendingDelay;

    return crossfading ? delayB : delayA;
}

void TempoDelay::startCrossfade (int newDelay) noexcept
{
    delayB = newDelay;
    crossfadePosition = 0;
    crossfading = true;
    pendingDelay = -1;
}

void TempoDelay::setTempo (double bpm, DelayDivision division) noexcept
{
    const auto target = delaySamplesFor (bpm, division);

    if (target == getTargetDelaySamples())
        return;

    if (crossfading)
        pendingDelay = target == delayB ? -1 : target; // wait for the running fade to finish
    else
        startCrossfade (target);
}

void TempoDelay::setFeedback (float value) noexcept
{
    feedback.setTargetValue (clampParameterValue (parameterIndex (GlobalControl::delayFeedback), value));
}

void TempoDelay::setReturn (float gain) noexcept
{
    returnGain.setTargetValue (clampParameterValue (parameterIndex (GlobalControl::delayReturn), gain));
}

void TempoDelay::reset() noexcept
{
    for (auto& line : lines)
        std::fill (line.begin(), line.end(), 0.0f);

    filters = {};
    writePosition = 0;

    const auto target = getTargetDelaySamples();
    delayA = delayB = target;
    pendingDelay = -1;
    crossfading = false;
    crossfadePosition = 0;

    feedback.setCurrentAndTargetValue (feedback.getTargetValue());
    returnGain.setCurrentAndTargetValue (returnGain.getTargetValue());
}

void TempoDelay::process (const float* const* send, float* const* wetOut, int numFrames) noexcept
{
    const juce::ScopedNoDenormals noDenormals;

    for (int i = 0; i < numFrames; ++i)
    {
        const auto fb = feedback.getNextValue();
        const auto level = returnGain.getNextValue();

        int readA = writePosition - delayA;
        if (readA < 0)
            readA += size;

        int readB = 0;
        double weightA = 1.0;
        double weightB = 0.0;

        if (crossfading)
        {
            readB = writePosition - delayB;
            if (readB < 0)
                readB += size;

            const auto angle = std::numbers::pi * 0.5 * crossfadePosition / crossfadeLength;
            weightA = std::cos (angle);
            weightB = std::sin (angle);
        }

        for (std::size_t c = 0; c < 2; ++c)
        {
            auto& line = lines[c];
            auto& filter = filters[c];

            double echo = static_cast<double> (line[static_cast<std::size_t> (readA)]) * weightA;
            if (crossfading)
                echo += static_cast<double> (line[static_cast<std::size_t> (readB)]) * weightB;

            // Loop colouring: DC blocker, then one-pole low-pass.
            const auto blocked = echo - filter.hpIn + dcBlockCoefficient * filter.hpOut;
            filter.hpIn = echo;
            filter.hpOut = blocked;
            filter.lp += lowPassCoefficient * (blocked - filter.lp);

            line[static_cast<std::size_t> (writePosition)] =
                sanitiseSample (send[c][i]) + static_cast<float> (static_cast<double> (fb) * filter.lp);
            wetOut[c][i] = static_cast<float> (echo) * level;
        }

        if (++writePosition >= size)
            writePosition = 0;

        if (crossfading && ++crossfadePosition >= crossfadeLength)
        {
            delayA = delayB;
            crossfading = false;

            if (pendingDelay >= 0 && pendingDelay != delayA)
                startCrossfade (pendingDelay);
            pendingDelay = -1;
        }
    }
}
} // namespace snds::dsp
