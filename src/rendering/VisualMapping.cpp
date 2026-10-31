#include "rendering/VisualMapping.h"

#include <algorithm>
#include <cmath>

namespace snds::rendering
{
namespace
{
constexpr float sideGain = 1.5f; // how quickly stacked panned stems saturate the side brightness
constexpr float shiftGain = 0.6f;
constexpr float snapDistance = 1.0e-4f;

float unit (float v) noexcept
{
    return std::isfinite (v) ? std::clamp (v, 0.0f, 1.0f) : 0.0f;
}
} // namespace

EffectParams targetEffects (const dsp::VisualSnapshot& snapshot, const VisualSettings& settings) noexcept
{
    const auto strength = std::isfinite (settings.intensity) ? std::clamp (settings.intensity, 0.0f, 1.0f) : 0.0f;

    if (settings.staticMode || ! settings.effectsEnabled || strength <= 0.0f)
        return {};

    EffectParams target;
    target.scale = 1.0f + limits::scale * strength * unit (snapshot.drumsPulse);
    target.depth = limits::depth * strength * unit (snapshot.bassDepth);
    target.glow = limits::glow * strength * unit (snapshot.vocalGlow);
    target.texture = limits::texture * strength * unit (snapshot.otherTexture);

    // Each stem pushes the side it is panned to, by how loud it is and how far it is panned.
    float left = 0.0f;
    float right = 0.0f;

    for (std::size_t i = 0; i < snapshot.pan.size(); ++i)
    {
        const auto pan = std::isfinite (snapshot.pan[i]) ? std::clamp (snapshot.pan[i], -1.0f, 1.0f) : 0.0f;
        const auto weight = unit (snapshot.level[i]) * std::abs (pan);

        if (pan < 0.0f)
            left += weight;
        else
            right += weight;
    }

    target.leftBrightness = limits::side * strength * (1.0f - std::exp (-sideGain * left));
    target.rightBrightness = limits::side * strength * (1.0f - std::exp (-sideGain * right));
    target.shift = limits::shift * strength * std::clamp ((right - left) * shiftGain, -1.0f, 1.0f);
    return target;
}

float easeValue (float current, float target, float neutral, const EasingTimes& times, double dtSeconds) noexcept
{
    const auto dt =
        std::clamp (static_cast<float> (std::isfinite (dtSeconds) ? dtSeconds : 0.0), 0.0f, easing::maxFrameSeconds);

    // Moving away from neutral is "rising"; coming back is "falling".
    const auto rising = std::abs (target - neutral) > std::abs (current - neutral);
    const auto tau = rising ? times.rise : times.fall;
    const auto alpha = tau > 0.0f ? 1.0f - std::exp (-dt / tau) : 1.0f;
    const auto next = current + (target - current) * alpha;
    return std::abs (target - next) < snapDistance ? target : next;
}

EffectParams VisualMapper::update (const dsp::VisualSnapshot& snapshot,
                                   const VisualSettings& settings,
                                   double dtSeconds) noexcept
{
    if (settings.staticMode)
    {
        state = {}; // Original A/B: never any motion, never any easing tail
        return state;
    }

    const auto target = targetEffects (snapshot, settings);

    state.scale = easeValue (state.scale, target.scale, 1.0f, easing::scale, dtSeconds);
    state.depth = easeValue (state.depth, target.depth, 0.0f, easing::depth, dtSeconds);
    state.glow = easeValue (state.glow, target.glow, 0.0f, easing::glow, dtSeconds);
    state.texture = easeValue (state.texture, target.texture, 0.0f, easing::texture, dtSeconds);
    state.leftBrightness = easeValue (state.leftBrightness, target.leftBrightness, 0.0f, easing::side, dtSeconds);
    state.rightBrightness = easeValue (state.rightBrightness, target.rightBrightness, 0.0f, easing::side, dtSeconds);
    state.shift = easeValue (state.shift, target.shift, 0.0f, easing::shift, dtSeconds);
    state.brightness = 1.0f;
    return state;
}
} // namespace snds::rendering
