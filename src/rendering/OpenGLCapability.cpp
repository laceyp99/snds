#include "rendering/OpenGLCapability.h"

namespace snds::rendering
{

namespace
{
/** Finds the first "<digits>.<digits>" in text; returns the two digit runs. */
bool findDottedNumber (const juce::String& text, juce::String& majorDigits, juce::String& minorDigits)
{
    const auto length = text.length();

    for (auto i = 0; i < length; ++i)
    {
        if (! juce::CharacterFunctions::isDigit (text[i]))
            continue;

        auto j = i;

        while (j < length && juce::CharacterFunctions::isDigit (text[j]))
            ++j;

        if (j + 1 < length && text[j] == '.' && juce::CharacterFunctions::isDigit (text[j + 1]))
        {
            auto k = j + 1;

            while (k < length && juce::CharacterFunctions::isDigit (text[k]))
                ++k;

            majorDigits = text.substring (i, j);
            minorDigits = text.substring (j + 1, k);
            return true;
        }

        i = j;
    }

    return false;
}
} // namespace

juce::String Version::toString() const
{
    return juce::String (major) + "." + juce::String (minor);
}

Version parseGlVersion (const juce::String& glVersion)
{
    juce::String majorDigits, minorDigits;

    if (! findDottedNumber (glVersion, majorDigits, minorDigits))
        return {};

    return { majorDigits.getIntValue(), minorDigits.getIntValue() };
}

Version parseGlslVersion (const juce::String& glslVersion)
{
    juce::String majorDigits, minorDigits;

    if (! findDottedNumber (glslVersion, majorDigits, minorDigits))
        return {};

    // GLSL minor versions are two digits ("1.50" == "#version 150"); normalise "1.5" to 1.50.
    auto minor = minorDigits.substring (0, 2).getIntValue();

    if (minorDigits.length() == 1)
        minor *= 10;

    return { majorDigits.getIntValue(), minor };
}

bool isSoftwareRenderer (const OpenGLInfo& info)
{
    const auto renderer = info.renderer.toLowerCase();

    return renderer.contains ("gdi generic") || renderer.contains ("llvmpipe") || renderer.contains ("softpipe")
           || renderer.contains ("swiftshader") || renderer.contains ("software rasterizer")
           || renderer.contains ("basic render driver");
}

CapabilityVerdict evaluateCapability (const OpenGLInfo& info, const CapabilityRequirements& requirements)
{
    CapabilityVerdict verdict;
    verdict.gl = parseGlVersion (info.version);
    verdict.glsl = parseGlslVersion (info.shadingLanguageVersion);
    verdict.softwareRenderer = isSoftwareRenderer (info);

    if (! verdict.gl.isValid())
        verdict.reasons.add ("GL_VERSION is missing or unreadable (\"" + info.version + "\").");
    else if (verdict.gl < requirements.minimumGl)
        verdict.reasons.add ("OpenGL " + verdict.gl.toString() + " is below the required "
                             + requirements.minimumGl.toString() + ".");

    if (! verdict.glsl.isValid())
        verdict.reasons.add ("GLSL is unavailable (GL_SHADING_LANGUAGE_VERSION is empty).");
    else if (verdict.glsl < requirements.minimumGlsl)
        verdict.reasons.add ("GLSL " + verdict.glsl.toString() + " is below the required "
                             + requirements.minimumGlsl.toString() + ".");

    if (requirements.rejectSoftwareRenderers && verdict.softwareRenderer)
        verdict.reasons.add ("\"" + info.renderer + "\" is a software renderer.");

    verdict.meetsMinimum = verdict.reasons.isEmpty();
    return verdict;
}

} // namespace snds::rendering
