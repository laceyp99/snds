#include "rendering/RenderTypes.h"

#include <algorithm>

namespace snds::rendering
{
juce::String tierName (RenderTier tier)
{
    switch (tier)
    {
        case RenderTier::gl:
            return "gl";
        case RenderTier::cpu:
            return "cpu";
        case RenderTier::staticImage:
            return "static";
        case RenderTier::none:
            break;
    }

    return "none";
}

juce::String causeName (FallbackCause cause)
{
    switch (cause)
    {
        case FallbackCause::disabledByRequest:
            return "disabled";
        case FallbackCause::contextCreationFailed:
            return "context-creation-failed";
        case FallbackCause::belowMinimumVersion:
            return "below-minimum-version";
        case FallbackCause::shaderCompileFailed:
            return "shader-compile-failed";
        case FallbackCause::resourceSetupFailed:
            return "resource-setup-failed";
        case FallbackCause::contextLost:
            return "context-lost";
        case FallbackCause::watchdogTimeout:
            return "watchdog-timeout";
        case FallbackCause::cpuOverBudget:
            return "cpu-over-budget";
        case FallbackCause::unavailable:
            return "unavailable";
        case FallbackCause::none:
            break;
    }

    return "none";
}

double renderClockMs() noexcept
{
    // Initialised on first use (from the message thread), never during static initialisation.
    static const double startMs = juce::Time::getMillisecondCounterHiRes();
    return juce::Time::getMillisecondCounterHiRes() - startMs;
}

ArtworkPalette derivePalette (const juce::Image& image)
{
    ArtworkPalette palette;

    if (! image.isValid())
        return palette;

    const juce::Image::BitmapData data (image, juce::Image::BitmapData::readOnly);
    constexpr int grid = 24;
    double red = 0.0, green = 0.0, blue = 0.0;
    int count = 0;
    float bestScore = -1.0f;

    for (int gy = 0; gy < grid; ++gy)
    {
        for (int gx = 0; gx < grid; ++gx)
        {
            const auto x = std::min (data.width - 1, (gx * data.width + data.width / 2) / grid);
            const auto y = std::min (data.height - 1, (gy * data.height + data.height / 2) / grid);
            const auto colour = data.getPixelColour (x, y);

            red += colour.getFloatRed();
            green += colour.getFloatGreen();
            blue += colour.getFloatBlue();
            ++count;

            // Saturated and bright beats merely bright: a vivid accent, not a white highlight.
            const auto score = colour.getSaturation() * colour.getBrightness();
            if (score > bestScore)
            {
                bestScore = score;
                palette.accent = colour.withAlpha (1.0f);
            }
        }
    }

    const auto n = static_cast<double> (std::max (1, count));
    palette.average = juce::Colour::fromFloatRGBA (
        static_cast<float> (red / n), static_cast<float> (green / n), static_cast<float> (blue / n), 1.0f);
    return palette;
}
} // namespace snds::rendering
