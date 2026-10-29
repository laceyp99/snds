#include "rendering/FallbackArtwork.h"

namespace snds::rendering
{

namespace
{
/** Hue in [0, 1), wrapped. */
float wrapHue (float hue) noexcept
{
    return hue - std::floor (hue);
}

juce::Colour hsv (float hue, float saturation, float brightness, float alpha = 1.0f)
{
    return juce::Colour::fromHSV (wrapHue (hue), saturation, brightness, alpha);
}
} // namespace

juce::uint64 fallbackArtworkSeed (const juce::String& text) noexcept
{
    constexpr juce::uint64 offsetBasis = 0xcbf29ce484222325ull;
    constexpr juce::uint64 prime = 0x100000001b3ull;

    auto hash = offsetBasis;
    const auto folded = text.toLowerCase();

    for (auto p = folded.toRawUTF8(); *p != 0; ++p)
    {
        hash ^= static_cast<juce::uint8> (*p);
        hash *= prime;
    }

    return hash;
}

juce::Image generateFallbackArtwork (juce::uint64 seed, int size)
{
    size = juce::jlimit (16, 4096, size);
    juce::Image image (juce::Image::ARGB, size, size, true, juce::SoftwareImageType());
    juce::Graphics g (image);
    juce::Random random (static_cast<juce::int64> (seed));

    const auto extent = static_cast<float> (size);
    const auto baseHue = random.nextFloat();
    const auto secondHue = baseHue + 0.07f + 0.13f * random.nextFloat(); // analogous
    const auto accentHue = baseHue + 0.42f + 0.16f * random.nextFloat(); // near-complementary

    // Background: a diagonal gradient from a deep base colour to a brighter analogous colour.
    const auto angle = random.nextFloat() * juce::MathConstants<float>::twoPi;
    const juce::Point<float> centre (extent * 0.5f, extent * 0.5f);
    const juce::Point<float> offset (std::cos (angle) * extent * 0.7f, std::sin (angle) * extent * 0.7f);
    g.setGradientFill (juce::ColourGradient (hsv (baseHue, 0.70f, 0.16f),
                                             centre - offset,
                                             hsv (secondHue, 0.62f, 0.48f),
                                             centre + offset,
                                             false));
    g.fillAll();

    // Soft light orbs.
    const auto numOrbs = 4 + random.nextInt (4);

    for (auto i = 0; i < numOrbs; ++i)
    {
        const auto hue = (i % 3 == 0) ? accentHue : (i % 2 == 0 ? secondHue : baseHue);
        const juce::Point<float> orbCentre (extent * (0.1f + 0.8f * random.nextFloat()),
                                            extent * (0.1f + 0.8f * random.nextFloat()));
        const auto radius = extent * (0.18f + 0.30f * random.nextFloat());
        const auto colour =
            hsv (hue + 0.03f * (random.nextFloat() - 0.5f), 0.55f, 0.95f, 0.30f + 0.20f * random.nextFloat());

        juce::ColourGradient glow (colour,
                                   orbCentre,
                                   colour.withAlpha (0.0f),
                                   orbCentre + juce::Point<float> (radius, 0.0f),
                                   true);
        g.setGradientFill (glow);
        g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (orbCentre));
    }

    // Concentric rings radiating from one point, like a sound wave.
    const juce::Point<float> ringCentre (extent * (0.25f + 0.5f * random.nextFloat()),
                                         extent * (0.25f + 0.5f * random.nextFloat()));
    const auto ringSpacing = extent * (0.035f + 0.03f * random.nextFloat());
    const auto ringColour = hsv (accentHue, 0.25f, 1.0f);

    for (auto ring = 1; ring * ringSpacing < extent * 1.2f; ++ring)
    {
        const auto radius = static_cast<float> (ring) * ringSpacing;
        const auto alpha = 0.16f * (1.0f - radius / (extent * 1.2f));
        g.setColour (ringColour.withAlpha (alpha));
        g.drawEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (ringCentre),
                       juce::jmax (1.0f, extent / 512.0f));
    }

    // Vignette keeps the edges calm and the centre readable.
    juce::ColourGradient vignette (juce::Colours::transparentBlack,
                                   centre,
                                   juce::Colours::black.withAlpha (0.45f),
                                   centre + juce::Point<float> (extent * 0.75f, 0.0f),
                                   true);
    g.setGradientFill (vignette);
    g.fillAll();

    return image;
}

} // namespace snds::rendering
