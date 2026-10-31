#pragma once

#include "rendering/RenderTypes.h"

namespace snds::rendering
{
/** Switches that force the failures the fallback ladder has to survive, for tests and for trying
    the fallbacks by hand. Nothing here changes behaviour unless a switch is set.

    From the environment, `SNDS_RENDER` holds a comma-separated list:
      no-opengl            never create a GL context (go straight to the CPU tier)
      shader-failure       compile a broken fragment shader
      context-loss[=ms]    the render thread reports a lost context after ms of rendering (1500)
      gl-hang[=ms]         diagnostic: block the render thread once for ms (4000)
      cpu-failure          declare the CPU tier over budget after 30 frames
      require-gl=M.m       the minimum accepted OpenGL version (default 3.2; 9.9 forces a failure)
*/
struct RenderOverrides
{
    bool forceNoOpenGL = false;
    bool forceShaderFailure = false;
    bool forceContextLoss = false;
    double contextLossAfterMs = 1500.0;
    bool forceGlHang = false;
    double glHangMs = 4000.0;
    bool forceCpuFailure = false;
    GlRequirement requirement;

    /** Parses the `SNDS_RENDER` list. Unknown items are ignored and reported in `unknown`. */
    static RenderOverrides parse (const juce::String& list, juce::StringArray* unknown = nullptr);

    /** Reads the `SNDS_RENDER` environment variable. */
    static RenderOverrides fromEnvironment();

    bool anySet() const noexcept
    {
        return forceNoOpenGL || forceShaderFailure || forceContextLoss || forceGlHang || forceCpuFailure
            || requirement.major != 3 || requirement.minor != 2;
    }
};
} // namespace snds::rendering
