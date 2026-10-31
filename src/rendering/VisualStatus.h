#pragma once

#include "rendering/RenderTypes.h"

#include <vector>

namespace snds::rendering
{
/** A short, plain-language reason for a fallback ("the graphics driver stopped responding"). */
juce::String causeSummary (FallbackCause cause);

/** The one-line detail the player shows under the artwork: which renderer is running and, when it
    is not the GPU, why. For example
      "Visuals: GPU, OpenGL 3.2 / GLSL 1.50 (Intel Iris Xe), 69 fps"
      "Visuals: CPU (no GPU effects), because the graphics shader failed to compile"
      "Visuals: still artwork, because OpenGL is not available"
    `staticMode` (the Original is playing) and `effectsEnabled` add a short note. */
juce::String describeVisuals (RenderTier tier,
                              const RendererStatus& status,
                              const std::vector<FallbackEvent>& events,
                              bool staticMode,
                              bool effectsEnabled);
} // namespace snds::rendering
