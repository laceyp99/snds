#pragma once

#include <juce_core/juce_core.h>

#include <compare>

/**
    OpenGL capability evaluation, kept free of OpenGL types so it can be unit tested and reused by the
    renderer, the GL probe spike, and diagnostics.

    Minimum tier for GPU shader visuals (resolves unknown-opengl-minimum):
    OpenGL 3.2 core profile with GLSL 1.50 on a hardware renderer. Anything else (context failure,
    lower versions, shader failure, or a software rasteriser such as Microsoft "GDI Generic") uses the
    CPU-light fallback, and ultimately static artwork.
*/
namespace snds::rendering
{

struct Version
{
    int major = 0;
    int minor = 0;

    bool isValid() const noexcept { return major > 0; }
    juce::String toString() const;

    friend auto operator<=> (const Version&, const Version&) = default;
};

/** Strings reported by glGetString plus the profile the context actually provides. */
struct OpenGLInfo
{
    juce::String vendor;                 ///< GL_VENDOR
    juce::String renderer;               ///< GL_RENDERER
    juce::String version;                ///< GL_VERSION
    juce::String shadingLanguageVersion; ///< GL_SHADING_LANGUAGE_VERSION (may be empty)
    bool coreProfile = false;
};

struct CapabilityRequirements
{
    Version minimumGl { 3, 2 };
    Version minimumGlsl { 1, 50 };
    bool rejectSoftwareRenderers = true;
};

struct CapabilityVerdict
{
    bool meetsMinimum = false;
    Version gl;
    Version glsl;
    bool softwareRenderer = false;
    juce::StringArray reasons; ///< Why the minimum is not met; empty when it is.
};

/** Parses GL_VERSION, e.g. "4.6.0 NVIDIA 560.94" -> 4.6, "OpenGL ES 3.2 ..." -> 3.2, "1.1.0" -> 1.1. */
Version parseGlVersion (const juce::String& glVersion);

/** Parses GL_SHADING_LANGUAGE_VERSION, e.g. "4.60 NVIDIA" -> 4.60, "1.50" -> 1.50, "1.5" -> 1.50. */
Version parseGlslVersion (const juce::String& glslVersion);

/** True for known software rasterisers (GDI Generic, llvmpipe, softpipe, SwiftShader, WARP). */
bool isSoftwareRenderer (const OpenGLInfo& info);

CapabilityVerdict evaluateCapability (const OpenGLInfo& info, const CapabilityRequirements& requirements = {});

} // namespace snds::rendering
