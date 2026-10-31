#include "rendering/VisualStatus.h"

#include "rendering/GlCapability.h"

namespace snds::rendering
{
juce::String causeSummary (FallbackCause cause)
{
    switch (cause)
    {
        case FallbackCause::disabledByRequest:
            return "it was switched off";
        case FallbackCause::contextCreationFailed:
            return "OpenGL is not available on this machine";
        case FallbackCause::belowMinimumVersion:
            return "OpenGL is older than 3.2";
        case FallbackCause::shaderCompileFailed:
            return "the graphics shader failed to compile";
        case FallbackCause::resourceSetupFailed:
            return "the graphics resources could not be set up";
        case FallbackCause::contextLost:
            return "the graphics context was lost";
        case FallbackCause::watchdogTimeout:
            return "the graphics driver stopped responding";
        case FallbackCause::cpuOverBudget:
            return "drawing was too slow";
        case FallbackCause::unavailable:
            return "that renderer is not available";
        case FallbackCause::none:
            break;
    }

    return {};
}

juce::String describeVisuals (RenderTier tier,
                              const RendererStatus& status,
                              const std::vector<FallbackEvent>& events,
                              bool staticMode,
                              bool effectsEnabled)
{
    juce::String text;

    switch (tier)
    {
        case RenderTier::gl:
        {
            text = "Visuals: GPU";

            if (status.gpu.contextCreated)
                text << ", " << describeGpu (status.gpu);

            if (status.averageFrameMs > 1.0)
                text << ", " << juce::String (juce::roundToInt (1000.0 / status.averageFrameMs)) << " fps";

            break;
        }
        case RenderTier::cpu:
            text = "Visuals: CPU (no GPU effects)";
            break;
        case RenderTier::staticImage:
            text = "Visuals: still artwork";
            break;
        case RenderTier::none:
            return "Visuals: starting";
    }

    // Why the best renderer is not the one in use: the first step down that mattered.
    for (const auto& event : events)
    {
        if (event.cause != FallbackCause::none && event.cause != FallbackCause::unavailable)
        {
            text << ", because " << causeSummary (event.cause);
            break;
        }
    }

    if (staticMode)
        text << " (the Original is playing, so the artwork stays still)";
    else if (! effectsEnabled && tier != RenderTier::staticImage)
        text << " (effects off)";

    return text;
}
} // namespace snds::rendering
