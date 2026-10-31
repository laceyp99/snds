#include "rendering/StaticRenderer.h"

#include <algorithm>

namespace snds::rendering
{
VisualRenderer::AttachResult StaticRenderer::attach (juce::Component& newHost)
{
    host = &newHost;
    attached = true;
    host->repaint();
    return {};
}

void StaticRenderer::detach()
{
    attached = false;
    host = nullptr;
}

void StaticRenderer::setArtwork (const ArtworkInput& newArtwork)
{
    artwork = newArtwork;

    if (host != nullptr)
        host->repaint();
}

juce::Rectangle<float> StaticRenderer::fitRectangle (juce::Rectangle<float> bounds, int imageWidth, int imageHeight)
{
    if (imageWidth <= 0 || imageHeight <= 0 || bounds.isEmpty())
        return {};

    const auto scale = std::min (bounds.getWidth() / static_cast<float> (imageWidth),
                                 bounds.getHeight() / static_cast<float> (imageHeight));
    return juce::Rectangle<float> (static_cast<float> (imageWidth) * scale, static_cast<float> (imageHeight) * scale)
        .withCentre (bounds.getCentre());
}

void StaticRenderer::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);

    if (host != nullptr && artwork.isValid())
    {
        const auto area =
            fitRectangle (host->getLocalBounds().toFloat(), artwork.image.getWidth(), artwork.image.getHeight());
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.drawImage (artwork.image, area, juce::RectanglePlacement::stretchToFit);
    }

    ++paints;
}

RendererStatus StaticRenderer::status() const
{
    RendererStatus s;
    s.tier = RenderTier::staticImage;
    s.attached = attached.load();
    s.ready = s.attached;
    s.frames = paints.load();
    return s;
}
} // namespace snds::rendering
