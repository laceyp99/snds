#pragma once

#include "rendering/RenderTypes.h"

#include <optional>
#include <utility>

namespace snds::rendering
{
/** The OpenGL capability check, split from the GL calls so it can be tested without a GPU.
    The GL renderer reads the driver strings after the context exists and passes them through
    here. Requesting 3.2 core is not a guarantee (JUCE falls back to a legacy context when
    wglCreateContextAttribsARB fails), so the version the driver reports is the only truth. */

/** "3.2.0 - Build 32.0.101.6733", "4.6.0 NVIDIA 560.94", "OpenGL ES 3.0 ..." -> {major, minor}. */
std::optional<std::pair<int, int>> parseGlVersion (const juce::String& versionString);

/** "1.50 - Build ...", "1.50 NVIDIA via Cg compiler", "4.60" -> 1.50, 1.50, 4.60. 0 if unparseable. */
double parseGlslVersion (const juce::String& glslString);

/** The GLSL version that ships with a core OpenGL version (3.2 -> 1.50; 3.3 and up share the number). */
double glslVersionFor (int major, int minor);

/** A requirement for OpenGL major.minor with the GLSL version that goes with it. */
GlRequirement requirementFor (int major, int minor);

/** Fills `meetsMinimum` and `verdict` from the version fields of `info`. */
void evaluateCapability (GpuInfo& info, const GlRequirement& requirement);

/** "OpenGL 3.2 / GLSL 1.50 (Intel Iris, Intel, core)": one line for logs and diagnostics. */
juce::String describeGpu (const GpuInfo& info);
} // namespace snds::rendering
