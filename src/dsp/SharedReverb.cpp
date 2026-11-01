#include "dsp/SharedReverb.h"

#include "dsp/MixParameters.h"
#include "dsp/Sanitize.h"

#include <algorithm>

namespace snds::dsp
{
namespace
{
// Freeverb's wet level is scaled internally; this makes a noise send come back at about 0.7x its
// level at a return of 1 (measured in DspReverbSendTests). Lower than unity on purpose: a tone
// near a comb resonance comes back several times louder, and the headroom budget needs that tamed.
constexpr float wetCalibration = 0.5f;
} // namespace

SharedReverb::SharedReverb()
{
    juce::Reverb::Parameters parameters;
    parameters.roomSize = 0.6f;
    parameters.damping = 0.5f;
    parameters.wetLevel = wetCalibration;
    parameters.dryLevel = 0.0f;
    parameters.width = 1.0f;
    parameters.freezeMode = 0.0f;
    reverb.setParameters (parameters);

    prepare (48000.0);
}

void SharedReverb::prepare (double sampleRate)
{
    reverb.setSampleRate (sampleRate);
    returnGain.reset (
        sampleRate,
        static_cast<double> (parameterSpec (parameterIndex (GlobalControl::reverbReturn)).smoothingSeconds));
    returnGain.setCurrentAndTargetValue (parameterSpec (parameterIndex (GlobalControl::reverbReturn)).defaultValue);
    reverb.reset();
}

void SharedReverb::setReturn (float gain) noexcept
{
    returnGain.setTargetValue (clampParameterValue (parameterIndex (GlobalControl::reverbReturn), gain));
}

void SharedReverb::reset() noexcept
{
    reverb.reset();
    returnGain.setCurrentAndTargetValue (returnGain.getTargetValue());
}

void SharedReverb::process (const float* const* send, float* const* wetOut, int numFrames) noexcept
{
    const juce::ScopedNoDenormals noDenormals;

    for (int i = 0; i < numFrames; ++i)
    {
        wetOut[0][i] = sanitiseSample (send[0][i]);
        wetOut[1][i] = sanitiseSample (send[1][i]);
    }

    reverb.processStereo (wetOut[0], wetOut[1], numFrames);

    if (! returnGain.isSmoothing())
    {
        const auto level = returnGain.getTargetValue();

        for (int i = 0; i < numFrames; ++i)
        {
            wetOut[0][i] *= level;
            wetOut[1][i] *= level;
        }
        return;
    }

    for (int i = 0; i < numFrames; ++i)
    {
        const auto level = returnGain.getNextValue();
        wetOut[0][i] *= level;
        wetOut[1][i] *= level;
    }
}
} // namespace snds::dsp
