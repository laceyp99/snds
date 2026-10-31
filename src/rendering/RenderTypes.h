#pragma once

#include <juce_graphics/juce_graphics.h>

#include <cstdint>

namespace snds::rendering
{
/** The renderer tiers, best first. A host only ever moves down this ladder. */
enum class RenderTier : int
{
    none,
    gl,         // GLSL fragment shader through OpenGL (render thread)
    cpu,        // lightweight juce::Graphics painting (message thread)
    staticImage // the artwork, no animation
};

juce::String tierName (RenderTier tier);

/** Why a renderer was left (or could not be used). Logged and shown, never fatal. */
enum class FallbackCause
{
    none,
    disabledByRequest,     // a forced-failure switch or the user turned a tier off
    contextCreationFailed, // no native OpenGL context
    belowMinimumVersion,   // the context is older than OpenGL 3.2 / GLSL 1.50
    shaderCompileFailed,   // GLSL compile or link error
    resourceSetupFailed,   // texture/buffer setup failed
    contextLost,           // GL_CONTEXT_LOST / out of memory (or a simulated loss)
    watchdogTimeout,       // the render thread stopped producing frames
    cpuOverBudget,         // painting is too slow for the message thread
    unavailable            // the tier is not implemented or not usable here
};

juce::String causeName (FallbackCause cause);

/** One step down the ladder, with the reason, for the log and for diagnostics. */
struct FallbackEvent
{
    RenderTier from = RenderTier::none;
    RenderTier to = RenderTier::none;
    FallbackCause cause = FallbackCause::none;
    juce::String detail; // human-readable, includes driver messages (for example the shader log)
    double atMs = 0.0;   // renderClockMs() when it happened
};

/** Milliseconds on a monotonic clock (first call is 0). Used for event times and watchdogs. */
double renderClockMs() noexcept;

//==============================================================================
/** The OpenGL tier the renderer requires: 3.2 core with GLSL 1.50 (spikes/opengl-tier). */
struct GlRequirement
{
    int major = 3;
    int minor = 2;
    double glsl = 1.50;

    juce::String versionString() const { return juce::String (major) + "." + juce::String (minor); }
};

/** What the driver reported after the context was created. Plain data: no GL types. */
struct GpuInfo
{
    bool contextCreated = false;
    juce::String vendor, renderer, version, glslVersion, profile;
    int major = 0;
    int minor = 0;
    double glslNumber = 0.0; // 1.50, 4.60, ...
    int maxTextureSize = 0;
    int contextFlags = 0;
    bool meetsMinimum = false;
    juce::String verdict; // empty when the minimum is met, otherwise why not
    double initMs = 0.0;  // render-thread time spent creating the shader and textures
};

//==============================================================================
/** The palette of a piece of artwork, for tinting glows. */
struct ArtworkPalette
{
    juce::Colour average = juce::Colours::grey;
    juce::Colour accent = juce::Colours::white;
};

/** The artwork a renderer shows. The image must not be modified after it is handed over. */
struct ArtworkInput
{
    juce::Image image;
    ArtworkPalette palette;

    bool isValid() const noexcept { return image.isValid(); }
};

/** Estimates a palette from an image: the average colour and the most saturated bright colour. */
ArtworkPalette derivePalette (const juce::Image& image);

/** The knobs the artwork effects turn, already mapped and bounded (see VisualMapping). The default
    leaves the artwork untouched, which is also what static mode and effects-off use. */
struct EffectParams
{
    float scale = 1.0f;           // >= 1: zoom about the centre (the drums pulse)
    float brightness = 1.0f;      // multiplies the colour (a global control; the mappings leave it at 1)
    float depth = 0.0f;           // 0..1: a slow push-in with darker edges (bass)
    float glow = 0.0f;            // 0..1: bloom (vocals)
    float texture = 0.0f;         // 0..1: fine grain (other)
    float leftBrightness = 0.0f;  // 0..1: extra brightness towards the left edge (panned left)
    float rightBrightness = 0.0f; // 0..1: extra brightness towards the right edge
    float shift = 0.0f;           // -1..1: slight horizontal drift towards the pan side

    bool isNeutral() const noexcept
    {
        return scale == 1.0f && brightness == 1.0f && depth == 0.0f && glow == 0.0f && texture == 0.0f
            && leftBrightness == 0.0f && rightBrightness == 0.0f && shift == 0.0f;
    }

    bool operator== (const EffectParams&) const = default;
};

/** A copy of a renderer's health, safe to take from the message thread at any time. */
struct RendererStatus
{
    RenderTier tier = RenderTier::none;
    bool attached = false;
    bool ready = false; // a frame has been produced (GL: the shader is built and drawing)
    GpuInfo gpu;        // only meaningful for the GL tier
    std::int64_t frames = 0;
    double msSinceLastFrame = -1.0; // -1 before the first frame
    double maxFrameGapMs = 0.0;
    double averageFrameMs = 0.0;    // GL: the time between frames (smoothed); CPU: the cost of a paint
    double msSinceHeartbeat = -1.0; // GL: the render thread is alive if this stays small; -1 = no such thread
    int glErrors = 0;
    FallbackCause lastFailure = FallbackCause::none;
    juce::String lastFailureDetail;
};
} // namespace snds::rendering
