#include "dsp/StemLevelStage.h"

#include <cmath>
#include <numbers>

namespace snds::dsp
{
std::array<bool, numStems> stemAudibility (const ParameterSnapshot& snapshot) noexcept
{
    std::array<bool, numStems> mute {};
    std::array<bool, numStems> solo {};

    for (int i = 0; i < numStems; ++i)
    {
        const auto stem = static_cast<Stem> (i);
        mute[static_cast<std::size_t> (i)] = snapshot.get (stem, StemControl::mute) >= 0.5f;
        solo[static_cast<std::size_t> (i)] = snapshot.get (stem, StemControl::solo) >= 0.5f;
    }

    return stemAudibility (mute, solo);
}

StemLevelStage::StemLevelStage() noexcept
{
    prepare (48000.0);
}

void StemLevelStage::prepare (double sampleRate) noexcept
{
    const auto spec = [] (StemControl control) { return parameterSpec (parameterIndex (Stem::drums, control)); };

    fader.reset (sampleRate, static_cast<double> (spec (StemControl::gain).smoothingSeconds));
    gate.reset (sampleRate, static_cast<double> (spec (StemControl::mute).smoothingSeconds));
    pan.reset (sampleRate, static_cast<double> (spec (StemControl::pan).smoothingSeconds));

    // Defaults: 0 dB, audible, centred.
    fader.setCurrentAndTargetValue (1.0f);
    gate.setCurrentAndTargetValue (1.0f);
    pan.setCurrentAndTargetValue (0.0f);
    panPosition.store (0.0f, std::memory_order_relaxed);
}

void StemLevelStage::setTargets (float gainDb, bool audible, float panValue) noexcept
{
    fader.setTargetValue (
        decibelsToGain (clampParameterValue (parameterIndex (Stem::drums, StemControl::gain), gainDb)));
    gate.setTargetValue (audible ? 1.0f : 0.0f);
    pan.setTargetValue (clampParameterValue (parameterIndex (Stem::drums, StemControl::pan), panValue));
}

void StemLevelStage::snapToTargets() noexcept
{
    fader.setCurrentAndTargetValue (fader.getTargetValue());
    gate.setCurrentAndTargetValue (gate.getTargetValue());
    pan.setCurrentAndTargetValue (pan.getTargetValue());
    panPosition.store (pan.getCurrentValue(), std::memory_order_relaxed);
}

std::array<float, 2> StemLevelStage::panGains (float position) noexcept
{
    position = std::clamp (position, panMin, panMax);

    if (position == 0.0f)
        return { 1.0f, 1.0f };

    const auto theta = (position + 1.0f) * (std::numbers::pi_v<float> * 0.25f);
    return { std::numbers::sqrt2_v<float> * std::cos (theta), std::numbers::sqrt2_v<float> * std::sin (theta) };
}

void StemLevelStage::process (const float* const* in, float* const* out, int numFrames) noexcept
{
    const float* inL = in[0];
    const float* inR = in[1];
    float* outL = out[0];
    float* outR = out[1];

    if (! (fader.isSmoothing() || gate.isSmoothing() || pan.isSmoothing()))
    {
        const auto level = fader.getTargetValue() * gate.getTargetValue();
        const auto [panL, panR] = panGains (pan.getTargetValue());
        const auto gainL = level * panL;
        const auto gainR = level * panR;

        for (int i = 0; i < numFrames; ++i)
        {
            outL[i] = inL[i] * gainL;
            outR[i] = inR[i] * gainR;
        }
    }
    else
    {
        for (int i = 0; i < numFrames; ++i)
        {
            const auto level = fader.getNextValue() * gate.getNextValue();
            const auto [panL, panR] = panGains (pan.getNextValue());
            outL[i] = inL[i] * (level * panL);
            outR[i] = inR[i] * (level * panR);
        }
    }

    panPosition.store (pan.getCurrentValue(), std::memory_order_relaxed);
}
} // namespace snds::dsp
