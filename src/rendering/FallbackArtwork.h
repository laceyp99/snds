#pragma once

#include <juce_graphics/juce_graphics.h>

/**
    Procedural fallback artwork for songs without usable artwork: an abstract, square image derived
    deterministically from a seed (normally the source file name), so the same song always gets the
    same picture and no binary asset ships with the app.
*/
namespace snds::rendering
{

/** Stable 64-bit seed (FNV-1a over the case-folded UTF-8 text). */
juce::uint64 fallbackArtworkSeed (const juce::String& text) noexcept;

/**
    Renders the fallback artwork: a two-hue gradient field, soft light orbs, concentric "sound"
    rings, and a vignette. Deterministic for a given seed and size. Returns an ARGB software image,
    so it is safe to call on any thread.
*/
juce::Image generateFallbackArtwork (juce::uint64 seed, int size = 1024);

} // namespace snds::rendering
