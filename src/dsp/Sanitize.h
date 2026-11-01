#pragma once

#include <cmath>

namespace snds::dsp
{
/** Replaces NaN, infinity and absurdly large values with silence. Used where a sample enters a
    feedback loop (delay, reverb), because a single NaN there would otherwise recirculate forever. */
inline float sanitiseSample (float x) noexcept
{
    return std::abs (x) <= 1.0e4f ? x : 0.0f; // false for NaN too
}
} // namespace snds::dsp
