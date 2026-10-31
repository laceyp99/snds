#pragma once

#include "rendering/RenderTypes.h"

#include <vector>

namespace snds::rendering
{
/** WCAG 2.x contrast ratio between two opaque colours: 1 (identical) to 21 (black on white).
    Text needs at least 4.5, large text and graphics at least 3. */
double contrastRatio (juce::Colour a, juce::Colour b);

/** WCAG relative luminance, 0 (black) to 1 (white). */
double relativeLuminance (juce::Colour c);

/** Moves `colour` towards white or black (whichever contrasts more with `background`), keeping its
    hue as long as it can, until it reaches `minimumRatio` against `background`. */
juce::Colour ensureContrast (juce::Colour colour, juce::Colour background, double minimumRatio);

/** The colours the player takes from the artwork, already checked against the colours they sit on.

    From artwork: `accent` is the artwork's dominant vivid colour, moved if needed so it reads as text
    or a thin line on `panelBackground` (ratio of at least 4.5); `onAccent` is black or white, whichever
    is readable on a filled `accent` (at least 4.5); `panelBackground` is the panel colour with a faint
    wash of the accent, never so much that the normal text falls under 7.
    Without usable artwork `fromArtwork` is false: `reason` says why, and the palette is the default
    styling (the plain panel colour and a fixed accent), which callers apply by not styling at all. */
struct ThemePalette
{
    juce::Colour accent;
    juce::Colour onAccent;
    juce::Colour panelBackground;
    bool fromArtwork = false;
    juce::String reason; // why the artwork was not used; empty when it was

    static constexpr double textContrast = 4.5;     // WCAG AA for normal text
    static constexpr double bodyTextContrast = 7.0; // the normal text on the tinted panel (AAA)
};

/** The default styling's palette on `panel` (used when the artwork is missing or unsuitable). */
ThemePalette defaultThemePalette (juce::Colour panel, juce::Colour text);

/** Builds the palette from artwork, for a panel of colour `panel` that shows `text`. Pure and
    deterministic. Missing, transparent, grey, near-black or near-white artwork gives the default
    palette with the reason. */
ThemePalette makeThemePalette (const juce::Image& artwork, juce::Colour panel, juce::Colour text);

/** What is wrong with a palette, one line each; empty when every rule holds. For tests and logs. */
std::vector<juce::String> validateThemePalette (const ThemePalette& palette, juce::Colour panel, juce::Colour text);
} // namespace snds::rendering
