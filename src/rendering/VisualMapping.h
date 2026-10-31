#pragma once

#include "dsp/VisualAnalysis.h"
#include "rendering/RenderTypes.h"

namespace snds::rendering
{
/** The conservative default strength of the effects (0 = none, 1 = the strongest allowed). */
inline constexpr float defaultVisualIntensity = 0.5f;

/** The user's visual choices. Intensity and effects-on are independent: switching effects off keeps
    the intensity for when they come back. Static mode is not a user setting but a state of the
    app: it is set while the Original is selected (A/B), and it wins over everything else. */
struct VisualSettings
{
    float intensity = defaultVisualIntensity; // 0..1, clamped
    bool effectsEnabled = true;
    bool staticMode = false; // Original A/B: the artwork must not move, whatever else is set
};

/** The strongest each effect gets, at intensity 1 and a full-scale analysis value. They keep the
    artwork recognisable: it never grows more than 5 %, glow and grain stay partial, and the
    brightness a panned side gains is a gentle ramp towards that edge only. */
namespace limits
{
inline constexpr float scale = 0.05f;  // drums pulse: up to +5 % zoom
inline constexpr float depth = 1.0f;   // bass: shader turns this into a 3 % push-in and 35 % darker edges
inline constexpr float glow = 0.8f;    // vocals
inline constexpr float texture = 0.6f; // other
inline constexpr float side = 1.0f;    // pan: shader scales it to +60 % at the edge
inline constexpr float shift = 1.0f;   // pan: shader scales it to 1.5 % of the width
} // namespace limits

/** The effect parameters one analysis snapshot asks for, before easing. Pure and deterministic:
    the same snapshot and settings always give the same parameters.

    The reactive mapping contract, from the post-fader analysis (so mute, solo, gain and filters
    change the picture because they change what is heard):
      - drums pulse -> scale (rhythmic pulses)
      - bass depth -> depth (scale and apparent depth)
      - vocal glow -> glow
      - other texture -> texture
      - pan -> leftBrightness / rightBrightness / shift: each stem pushes brightness towards the
        side it is panned to, weighted by its level (a silent stem or a centred one adds nothing)
    Static mode, effects off, intensity 0 and a silent mix all give exactly the neutral parameters. */
EffectParams targetEffects (const dsp::VisualSnapshot& snapshot, const VisualSettings& settings) noexcept;

/** How fast each effect follows its target (seconds). Rising is quicker than falling, so a hit
    reads as a flick and fades like one. */
struct EasingTimes
{
    float rise = 0.1f;
    float fall = 0.3f;
};

namespace easing
{
inline constexpr EasingTimes scale { 0.020f, 0.100f };
inline constexpr EasingTimes depth { 0.150f, 0.400f };
inline constexpr EasingTimes glow { 0.100f, 0.300f };
inline constexpr EasingTimes texture { 0.100f, 0.300f };
inline constexpr EasingTimes side { 0.200f, 0.350f };
inline constexpr EasingTimes shift { 0.200f, 0.350f };

/** Frame hitches are clamped to this, so one long frame cannot make the picture jump. */
inline constexpr float maxFrameSeconds = 0.25f;
} // namespace easing

/** Eases the parameters towards their targets, once per rendered frame. The easing is a
    first-order lag with a time constant in seconds, so the result depends on elapsed time, not on
    the frame rate: 60 and 144 fps follow the same curve. A value that gets within 1e-4 of its
    target snaps to it, so neutral is reached exactly. Static mode skips the easing and is neutral
    immediately.

    Message thread (or whichever thread draws the frame). Not shared. */
class VisualMapper
{
public:
    /** The parameters for the next frame, `dtSeconds` after the previous one. */
    EffectParams update (const dsp::VisualSnapshot& snapshot,
                         const VisualSettings& settings,
                         double dtSeconds) noexcept;

    const EffectParams& current() const noexcept { return state; }

    /** Back to neutral without easing (a new song, a seek). */
    void reset() noexcept { state = {}; }

private:
    EffectParams state;
};

/** One easing step for a single value (exposed for tests). */
float easeValue (float current, float target, float neutral, const EasingTimes& times, double dtSeconds) noexcept;
} // namespace snds::rendering
