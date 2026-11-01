#include "rendering/GlCapability.h"

#include <cmath>

namespace snds::rendering
{
namespace
{
// The first "digits.digits" in the text, as the digit runs on either side of the dot.
bool findNumber (const juce::String& text, juce::String& whole, juce::String& fraction)
{
    const auto length = text.length();

    for (int i = 0; i < length; ++i)
    {
        if (! juce::CharacterFunctions::isDigit (text[i]))
            continue;

        int end = i;
        while (end < length && juce::CharacterFunctions::isDigit (text[end]))
            ++end;

        if (end < length && text[end] == '.' && end + 1 < length && juce::CharacterFunctions::isDigit (text[end + 1]))
        {
            int fractionEnd = end + 1;
            while (fractionEnd < length && juce::CharacterFunctions::isDigit (text[fractionEnd]))
                ++fractionEnd;

            whole = text.substring (i, end);
            fraction = text.substring (end + 1, fractionEnd);
            return true;
        }

        i = end; // skip this run of digits ("2 3.2" finds 3.2)
    }

    return false;
}
} // namespace

std::optional<std::pair<int, int>> parseGlVersion (const juce::String& versionString)
{
    juce::String whole, fraction;

    if (! findNumber (versionString, whole, fraction))
        return std::nullopt;

    return std::pair { whole.getIntValue(), fraction.getIntValue() };
}

double parseGlslVersion (const juce::String& glslString)
{
    juce::String whole, fraction;

    if (! findNumber (glslString, whole, fraction))
        return 0.0;

    // "1.50" and "1.5" are the same version; "4.60" is 4.6.
    return static_cast<double> (whole.getIntValue())
         + static_cast<double> (fraction.getIntValue()) / std::pow (10.0, fraction.length());
}

double glslVersionFor (int major, int minor)
{
    const auto gl = major * 10 + minor;

    if (gl <= 20)
        return 1.10;
    if (gl == 21)
        return 1.20;
    if (gl == 30)
        return 1.30;
    if (gl == 31)
        return 1.40;
    if (gl == 32)
        return 1.50;
    return static_cast<double> (major) + static_cast<double> (minor) / 10.0;
}

GlRequirement requirementFor (int major, int minor)
{
    GlRequirement requirement;
    requirement.major = major;
    requirement.minor = minor;
    requirement.glsl = glslVersionFor (major, minor);
    return requirement;
}

void evaluateCapability (GpuInfo& info, const GlRequirement& requirement)
{
    if (! info.contextCreated)
    {
        info.meetsMinimum = false;
        info.verdict = "no OpenGL context";
        return;
    }

    const auto versionOk = info.major * 10 + info.minor >= requirement.major * 10 + requirement.minor;
    const auto glslOk = info.glslNumber + 1.0e-6 >= requirement.glsl;
    info.meetsMinimum = versionOk && glslOk;

    if (info.meetsMinimum)
    {
        info.verdict = {};
        return;
    }

    info.verdict = "OpenGL " + juce::String (info.major) + "." + juce::String (info.minor) + " / GLSL "
                 + juce::String (info.glslNumber, 2) + " is below the required OpenGL " + requirement.versionString()
                 + " / GLSL " + juce::String (requirement.glsl, 2);
}

juce::String describeGpu (const GpuInfo& info)
{
    if (! info.contextCreated)
        return "no OpenGL context";

    return "OpenGL " + juce::String (info.major) + "." + juce::String (info.minor) + " / GLSL "
         + juce::String (info.glslNumber, 2) + " (" + info.renderer + ", " + info.vendor
         + (info.profile.isNotEmpty() ? ", " + info.profile : juce::String()) + ")";
}
} // namespace snds::rendering
