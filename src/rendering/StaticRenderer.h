#pragma once

#include "rendering/VisualRenderer.h"

#include <atomic>

namespace snds::rendering
{
/** The bottom of the ladder: the artwork, fitted into the host, with no animation and no GPU.
    It cannot fail. Effect parameters are ignored, which is also what static mode (Original A/B)
    wants from every tier.

    Message thread only, except status().
*/
class StaticRenderer final : public VisualRenderer
{
public:
    StaticRenderer() = default;

    RenderTier tier() const noexcept override { return RenderTier::staticImage; }

    AttachResult attach (juce::Component& host) override;
    void detach() override;

    void setArtwork (const ArtworkInput& newArtwork) override;
    void setEffects (const EffectParams&) override {}

    void paint (juce::Graphics& g) override;
    void tick() override {}

    RendererStatus status() const override;
    void setFailureCallback (FailureCallback) override {}

    /** The rectangle (inside `bounds`) the artwork occupies when it is fitted without cropping. */
    static juce::Rectangle<float> fitRectangle (juce::Rectangle<float> bounds, int imageWidth, int imageHeight);

private:
    juce::Component* host = nullptr;
    ArtworkInput artwork;
    std::atomic<bool> attached { false };
    std::atomic<std::int64_t> paints { 0 };
};
} // namespace snds::rendering
