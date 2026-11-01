#include "rendering/ArtworkPalette.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace snds::rendering
{
namespace
{
constexpr int gridSize = 32;
constexpr int hueBins = 12;
constexpr float minimumSaturation = 0.20f;     // a sample needs some colour to count
constexpr float minimumBrightness = 0.20f;     // and some light
constexpr double minimumColourfulShare = 0.03; // of the opaque samples, or the art has no distinct colour
constexpr double minimumOpaqueShare = 0.25;
constexpr float accentSaturationFloor = 0.45f;
constexpr float tintAmount = 0.12f;

double channelToLinear (float c)
{
    return c <= 0.03928f ? static_cast<double> (c) / 12.92 : std::pow ((static_cast<double> (c) + 0.055) / 1.055, 2.4);
}

juce::Colour opaque (juce::Colour c)
{
    return c.withAlpha (1.0f);
}
} // namespace

double relativeLuminance (juce::Colour c)
{
    return 0.2126 * channelToLinear (c.getFloatRed()) + 0.7152 * channelToLinear (c.getFloatGreen())
         + 0.0722 * channelToLinear (c.getFloatBlue());
}

double contrastRatio (juce::Colour a, juce::Colour b)
{
    const auto la = relativeLuminance (a);
    const auto lb = relativeLuminance (b);
    return (std::max (la, lb) + 0.05) / (std::min (la, lb) + 0.05);
}

juce::Colour ensureContrast (juce::Colour colour, juce::Colour background, double minimumRatio)
{
    colour = opaque (colour);

    if (contrastRatio (colour, background) >= minimumRatio)
        return colour;

    // Go the way that has room: towards white on a dark background, towards black on a light one,
    // and in between towards whichever extreme contrasts more.
    const auto towardsWhite =
        contrastRatio (juce::Colours::white, background) >= contrastRatio (juce::Colours::black, background);

    auto result = colour;

    // First keep the hue and saturation and change only the brightness.
    for (int step = 0; step < 50 && contrastRatio (result, background) < minimumRatio; ++step)
    {
        const auto brightness = result.getBrightness() + (towardsWhite ? 0.02f : -0.02f);
        result = juce::Colour::fromHSV (
            result.getHue(), result.getSaturation(), juce::jlimit (0.0f, 1.0f, brightness), 1.0f);
    }

    // Then, if the hue cannot get there alone, blend towards the extreme.
    for (int step = 1; step <= 20 && contrastRatio (result, background) < minimumRatio; ++step)
        result = result.interpolatedWith (towardsWhite ? juce::Colours::white : juce::Colours::black, 0.1f);

    return opaque (result);
}

ThemePalette defaultThemePalette (juce::Colour panel, juce::Colour text)
{
    juce::ignoreUnused (text);

    ThemePalette palette;
    palette.panelBackground = opaque (panel);
    palette.accent = ensureContrast (juce::Colour (0xff4c9aff), panel, ThemePalette::textContrast);
    palette.onAccent =
        contrastRatio (juce::Colours::white, palette.accent) >= contrastRatio (juce::Colours::black, palette.accent)
            ? juce::Colours::white
            : juce::Colours::black;
    palette.fromArtwork = false;
    return palette;
}

ThemePalette makeThemePalette (const juce::Image& artwork, juce::Colour panel, juce::Colour text)
{
    auto fallback = [&] (const juce::String& reason)
    {
        auto palette = defaultThemePalette (panel, text);
        palette.reason = reason;
        return palette;
    };

    if (! artwork.isValid() || artwork.getWidth() < 1 || artwork.getHeight() < 1)
        return fallback ("there is no artwork");

    const juce::Image::BitmapData data (artwork, juce::Image::BitmapData::readOnly);

    struct Bin
    {
        double weight = 0.0, red = 0.0, green = 0.0, blue = 0.0;
    };

    std::array<Bin, hueBins> bins;
    int opaqueSamples = 0;
    int colourfulSamples = 0;

    for (int gy = 0; gy < gridSize; ++gy)
    {
        for (int gx = 0; gx < gridSize; ++gx)
        {
            const auto x = std::min (data.width - 1, (gx * data.width + data.width / 2) / gridSize);
            const auto y = std::min (data.height - 1, (gy * data.height + data.height / 2) / gridSize);
            const auto pixel = data.getPixelColour (x, y);

            if (pixel.getAlpha() < 128)
                continue;

            ++opaqueSamples;

            if (pixel.getSaturation() < minimumSaturation || pixel.getBrightness() < minimumBrightness)
                continue;

            ++colourfulSamples;
            const auto weight =
                static_cast<double> (pixel.getSaturation()) * (0.25 + static_cast<double> (pixel.getBrightness()));
            auto& bin =
                bins[static_cast<std::size_t> (std::min (hueBins - 1, static_cast<int> (pixel.getHue() * hueBins)))];
            bin.weight += weight;
            bin.red += weight * static_cast<double> (pixel.getFloatRed());
            bin.green += weight * static_cast<double> (pixel.getFloatGreen());
            bin.blue += weight * static_cast<double> (pixel.getFloatBlue());
        }
    }

    const auto samples = gridSize * gridSize;

    if (static_cast<double> (opaqueSamples) < minimumOpaqueShare * samples)
        return fallback ("the artwork is mostly transparent");

    if (static_cast<double> (colourfulSamples) < minimumColourfulShare * opaqueSamples)
        return fallback ("the artwork has no distinct colour (it is grey, very dark or very light)");

    // The dominant vivid colour: the heaviest hue, as the weighted mean of its pixels.
    const auto best =
        std::max_element (bins.begin(), bins.end(), [] (const Bin& a, const Bin& b) { return a.weight < b.weight; });
    auto accent = juce::Colour::fromFloatRGBA (static_cast<float> (best->red / best->weight),
                                               static_cast<float> (best->green / best->weight),
                                               static_cast<float> (best->blue / best->weight),
                                               1.0f);
    accent = juce::Colour::fromHSV (accent.getHue(),
                                    std::max (accent.getSaturation(), accentSaturationFloor),
                                    std::max (accent.getBrightness(), 0.5f),
                                    1.0f);

    ThemePalette palette;
    palette.fromArtwork = true;

    // A faint wash of the accent in the panel, as much as keeps the normal text at 7:1.
    palette.panelBackground = opaque (panel);

    for (const auto amount : { tintAmount, tintAmount * 0.5f, tintAmount * 0.25f })
    {
        const auto tinted = opaque (panel.interpolatedWith (accent, amount));

        if (contrastRatio (text, tinted) >= ThemePalette::bodyTextContrast)
        {
            palette.panelBackground = tinted;
            break;
        }
    }

    palette.accent = ensureContrast (accent, palette.panelBackground, ThemePalette::textContrast);
    palette.onAccent =
        contrastRatio (juce::Colours::white, palette.accent) >= contrastRatio (juce::Colours::black, palette.accent)
            ? juce::Colours::white
            : juce::Colours::black;
    return palette;
}

std::vector<juce::String> validateThemePalette (const ThemePalette& palette, juce::Colour panel, juce::Colour text)
{
    std::vector<juce::String> problems;

    if (contrastRatio (palette.accent, palette.panelBackground) < ThemePalette::textContrast - 1.0e-9)
        problems.push_back ("the accent is under 4.5:1 on the panel ("
                            + juce::String (contrastRatio (palette.accent, palette.panelBackground), 2) + ")");

    if (contrastRatio (palette.onAccent, palette.accent) < ThemePalette::textContrast - 1.0e-9)
        problems.push_back ("text on the accent is under 4.5:1 ("
                            + juce::String (contrastRatio (palette.onAccent, palette.accent), 2) + ")");

    // The wash must not cost the normal text its contrast (when the plain panel had it to begin with).
    if (palette.fromArtwork && contrastRatio (text, panel) >= ThemePalette::bodyTextContrast
        && contrastRatio (text, palette.panelBackground) < ThemePalette::bodyTextContrast - 1.0e-9)
        problems.push_back ("the normal text is under 7:1 on the tinted panel ("
                            + juce::String (contrastRatio (text, palette.panelBackground), 2) + ")");

    if (! palette.fromArtwork && palette.panelBackground != opaque (panel))
        problems.push_back ("the default palette changed the panel colour");

    if (palette.fromArtwork && palette.reason.isNotEmpty())
        problems.push_back ("a palette from artwork carries a fallback reason");

    return problems;
}
} // namespace snds::rendering
