#pragma once

#include "rendering/VisualHost.h"
#include "rendering/VisualMapping.h"

#include <algorithm>

namespace snds::rendering
{
/** Connects the analysis to the picture: each UI tick it takes the latest analysis snapshot, maps
    and eases it (VisualMapper) and hands the effect parameters to a VisualHost.

    A panel owns one of these next to its VisualHost, calls update() from its timer, and passes the
    user's choices through. The Original/Stem Mix switch is setOriginalSelected: while the Original
    plays the artwork is static, whatever the intensity and effects settings say.

    Message thread only. Never touches audio: it reads the lock-free VisualStore the audio thread
    publishes.
*/
class VisualController
{
public:
    explicit VisualController (VisualHost& hostToDrive) noexcept
        : host (hostToDrive)
    {
    }

    /** 0..1; clamped. */
    void setIntensity (float intensity) noexcept { settings.intensity = std::clamp (intensity, 0.0f, 1.0f); }
    void setEffectsEnabled (bool enabled) noexcept { settings.effectsEnabled = enabled; }

    /** True while the Original (A/B) is selected: forces static artwork at once. */
    void setOriginalSelected (bool original);

    const VisualSettings& getSettings() const noexcept { return settings; }

    /** One UI tick with the newest analysis. `nowMs` is any monotonic clock in milliseconds; the
        easing uses the time since the previous call. */
    void update (const dsp::VisualSnapshot& snapshot, double nowMs);

    /** The same, reading the snapshot from the audio thread's store (a failed read keeps the last one). */
    void update (const dsp::VisualStore& store, double nowMs);

    /** The picture goes back to neutral without easing (a new song, a seek, a reset). */
    void reset();

private:
    VisualHost& host;
    VisualSettings settings;
    VisualMapper mapper;
    dsp::VisualSnapshot latest;
    double lastMs = -1.0;
};
} // namespace snds::rendering
