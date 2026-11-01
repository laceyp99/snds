#pragma once

#include "rendering/RenderOverrides.h"
#include "rendering/VisualRenderer.h"

#include <atomic>
#include <deque>
#include <functional>

namespace snds::rendering
{
/** The CPU-light tier: the artwork and a few cheap juce::Graphics effects, painted on the message
    thread. It is what remains when OpenGL is missing, too old, or has failed, and it keeps the
    show reactive without a GPU.

    Effects (a lightweight version of the shader): zoom and drift, a push-in with darker edges
    (bass), a tinted glow (vocals), brighter side edges (pan) and a global brightness. The fine grain
    (other) is left out, because a per-pixel pass is not light.

    Paint cost is watched. If the average paint over 30 paints exceeds 8 ms the tier drops from 60
    to 30 frames per second; if it still averages more than 12 ms it reports FallbackCause::cpuOverBudget
    (once) and the host moves on to the static tier. Message thread only, except status().
*/
class CpuRenderer final : public VisualRenderer
{
public:
    /** A millisecond clock; tests inject one to make paints look slow. */
    using Clock = std::function<double()>;

    explicit CpuRenderer (RenderOverrides overrides = {}, Clock clock = {});

    RenderTier tier() const noexcept override { return RenderTier::cpu; }

    AttachResult attach (juce::Component& host) override;
    void detach() override;

    void setArtwork (const ArtworkInput& newArtwork) override { artwork = newArtwork; }
    void setEffects (const EffectParams& newEffects) override { effects = newEffects; }

    void paint (juce::Graphics& g) override;
    void tick() override;

    RendererStatus status() const override;
    void setFailureCallback (FailureCallback callback) override { onFailure = std::move (callback); }
    double preferredTickHz() const noexcept override;

    /** True once the tier has dropped to 30 Hz because paints were slow. */
    bool isReduced() const noexcept { return reduced; }

    /** One frame into `bounds` (exposed for tests). */
    static void drawFrame (juce::Graphics& g,
                           juce::Rectangle<float> bounds,
                           const ArtworkInput& artwork,
                           const EffectParams& effects);

private:
    void fail (FallbackCause cause, const juce::String& detail);

    RenderOverrides overrides;
    Clock clock;
    juce::Component* host = nullptr;
    ArtworkInput artwork;
    EffectParams effects;
    FailureCallback onFailure;

    std::deque<double> recentPaintMs;
    bool reduced = false;
    bool failed = false;
    FallbackCause lastFailure = FallbackCause::none;
    juce::String lastFailureDetail;

    std::atomic<bool> attached { false };
    std::atomic<std::int64_t> frames { 0 };
    std::atomic<double> averagePaintMs { 0.0 };
    std::atomic<double> lastPaintAtMs { -1.0 };
};
} // namespace snds::rendering
