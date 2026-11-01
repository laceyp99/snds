#include "dsp/MasterStage.h"

#include "dsp/MixParameters.h"
#include "dsp/Sanitize.h"

#include <algorithm>
#include <cmath>

namespace snds::dsp
{
namespace
{
constexpr double limiterReleaseSeconds = 0.1;
constexpr float snapToUnity = 1.0e-5f;
// The exponential release alone would stall just short of 1 in float; this floor lets it arrive.
constexpr float minimumReleaseStep = 1.0e-6f;

int masterGainIndex()
{
    return parameterIndex (GlobalControl::masterGain);
}
} // namespace

MasterStage::MasterStage() noexcept
{
    prepare (48000.0);
}

void MasterStage::prepare (double sampleRate) noexcept
{
    gain.reset (sampleRate, static_cast<double> (parameterSpec (masterGainIndex()).smoothingSeconds));
    gain.setCurrentAndTargetValue (decibelsToGain (parameterSpec (masterGainIndex()).defaultValue));
    releaseCoefficient = static_cast<float> (1.0 - std::exp (-1.0 / (limiterReleaseSeconds * sampleRate)));
    limiterGain = 1.0f;
}

void MasterStage::setGainDb (float decibels) noexcept
{
    gain.setTargetValue (decibelsToGain (clampParameterValue (masterGainIndex(), decibels)));
}

void MasterStage::reset() noexcept
{
    gain.setCurrentAndTargetValue (gain.getTargetValue());
    limiterGain = 1.0f;
}

MasterStage::BlockInfo MasterStage::process (const float* const* in, float* const* out, int numFrames) noexcept
{
    BlockInfo info;
    float g = limiterGain;

    for (int i = 0; i < numFrames; ++i)
    {
        const auto master = gain.getNextValue();
        auto left = sanitiseSample (in[0][i]) * master;
        auto right = sanitiseSample (in[1][i]) * master;

        const auto peak = std::max (std::abs (left), std::abs (right));
        info.inputPeak = std::max (info.inputPeak, peak);

        const auto needed = peak > limiterCeiling ? limiterCeiling / peak : 1.0f;

        if (needed < g)
        {
            g = needed; // instant attack: never above the ceiling, never delayed
        }
        else if (g < 1.0f)
        {
            g += std::max ((1.0f - g) * releaseCoefficient, minimumReleaseStep);
            g = std::min (g, needed);

            if (needed >= 1.0f && (1.0f - g) < snapToUnity)
                g = 1.0f;
        }

        if (g < 1.0f)
        {
            left = std::clamp (left * g, -limiterCeiling, limiterCeiling);
            right = std::clamp (right * g, -limiterCeiling, limiterCeiling);
            info.minGain = std::min (info.minGain, g);
        }

        out[0][i] = left;
        out[1][i] = right;
    }

    limiterGain = g;
    return info;
}
} // namespace snds::dsp
