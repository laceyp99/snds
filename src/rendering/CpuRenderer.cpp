#include "rendering/CpuRenderer.h"

#include "rendering/FramePacing.h"
#include "rendering/StaticRenderer.h"

#include <numeric>

namespace snds::rendering
{
CpuRenderer::CpuRenderer (RenderOverrides o, Clock c)
    : overrides (std::move (o)),
      clock (c ? std::move (c) : Clock (renderClockMs))
{
}

VisualRenderer::AttachResult CpuRenderer::attach (juce::Component& newHost)
{
    host = &newHost;
    attached = true;
    host->repaint();
    return {};
}

void CpuRenderer::detach()
{
    attached = false;
    host = nullptr;
}

double CpuRenderer::preferredTickHz() const noexcept
{
    return reduced ? pacing::cpuReducedHz : pacing::cpuFullHz;
}

void CpuRenderer::drawFrame (juce::Graphics& g,
                             juce::Rectangle<float> bounds,
                             const ArtworkInput& artwork,
                             const EffectParams& effects)
{
    g.fillAll (juce::Colours::black);

    if (! artwork.isValid())
        return;

    const auto fit = StaticRenderer::fitRectangle (bounds, artwork.image.getWidth(), artwork.image.getHeight());

    if (fit.isEmpty())
        return;

    const juce::Graphics::ScopedSaveState state (g);
    g.reduceClipRegion (fit.getSmallestIntegerContainer());

    // Zoom and push-in grow the picture inside its own frame, drift moves it a little sideways.
    const auto zoom = juce::jmax (1.0f, effects.scale) * (1.0f + 0.03f * effects.depth);
    const auto dest = fit.withSizeKeepingCentre (fit.getWidth() * zoom, fit.getHeight() * zoom)
                          .translated (effects.shift * 0.015f * fit.getWidth(), 0.0f);

    g.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);
    g.drawImage (artwork.image, dest, juce::RectanglePlacement::stretchToFit);

    if (effects.glow > 0.0f)
    {
        g.setGradientFill (juce::ColourGradient (artwork.palette.accent.withAlpha (0.45f * effects.glow),
                                                 fit.getCentre(),
                                                 artwork.palette.accent.withAlpha (0.0f),
                                                 fit.getTopLeft(),
                                                 true));
        g.fillRect (fit);
    }

    if (effects.depth > 0.0f)
    {
        g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.0f),
                                                 fit.getCentre(),
                                                 juce::Colours::black.withAlpha (0.35f * effects.depth),
                                                 fit.getTopLeft(),
                                                 true));
        g.fillRect (fit);
    }

    const auto sideWidth = fit.getWidth() * 0.45f;

    if (effects.leftBrightness > 0.0f)
    {
        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.3f * effects.leftBrightness),
                                                 fit.getX(),
                                                 0.0f,
                                                 juce::Colours::white.withAlpha (0.0f),
                                                 fit.getX() + sideWidth,
                                                 0.0f,
                                                 false));
        g.fillRect (fit);
    }

    if (effects.rightBrightness > 0.0f)
    {
        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.3f * effects.rightBrightness),
                                                 fit.getRight(),
                                                 0.0f,
                                                 juce::Colours::white.withAlpha (0.0f),
                                                 fit.getRight() - sideWidth,
                                                 0.0f,
                                                 false));
        g.fillRect (fit);
    }

    if (effects.brightness != 1.0f)
    {
        g.setColour ((effects.brightness > 1.0f ? juce::Colours::white : juce::Colours::black)
                         .withAlpha (juce::jlimit (0.0f, 1.0f, std::abs (effects.brightness - 1.0f) * 0.5f)));
        g.fillRect (fit);
    }
}

void CpuRenderer::paint (juce::Graphics& g)
{
    if (host == nullptr)
        return;

    const auto start = clock();
    drawFrame (g, host->getLocalBounds().toFloat(), artwork, effects);
    const auto elapsed = clock() - start;

    recentPaintMs.push_back (elapsed);

    while (static_cast<int> (recentPaintMs.size()) > pacing::cpuBudgetWindow)
        recentPaintMs.pop_front();

    averagePaintMs =
        std::accumulate (recentPaintMs.begin(), recentPaintMs.end(), 0.0) / static_cast<double> (recentPaintMs.size());
    lastPaintAtMs = start;
    ++frames;
}

void CpuRenderer::fail (FallbackCause cause, const juce::String& detail)
{
    if (failed)
        return;

    failed = true;
    lastFailure = cause;
    lastFailureDetail = detail;

    if (onFailure)
        onFailure (cause, detail);
}

void CpuRenderer::tick()
{
    if (failed)
        return;

    if (overrides.forceCpuFailure && frames.load() >= pacing::cpuBudgetWindow)
    {
        fail (FallbackCause::cpuOverBudget,
              "CPU tier over budget after " + juce::String (pacing::cpuBudgetWindow)
                  + " frames (forced by the render overrides)");
        return;
    }

    if (static_cast<int> (recentPaintMs.size()) < pacing::cpuBudgetWindow)
        return;

    const auto average = averagePaintMs.load();
    const auto budget = reduced ? pacing::cpuReducedBudgetMs : pacing::cpuBudgetMs;

    if (average <= budget)
        return;

    if (! reduced)
    {
        // First answer to slow paints: fewer of them, not an immediate surrender.
        reduced = true;
        recentPaintMs.clear();
        averagePaintMs = 0.0;
        juce::Logger::writeToLog ("visuals: cpu paints average " + juce::String (average, 1) + " ms; dropping to "
                                  + juce::String (juce::roundToInt (pacing::cpuReducedHz)) + " Hz");
        return;
    }

    fail (FallbackCause::cpuOverBudget,
          "CPU tier over budget: paints average " + juce::String (average, 1) + " ms at "
              + juce::String (juce::roundToInt (pacing::cpuReducedHz)) + " Hz");
}

RendererStatus CpuRenderer::status() const
{
    RendererStatus s;
    s.tier = RenderTier::cpu;
    s.attached = attached.load();
    s.ready = frames.load() > 0 && ! failed;
    s.frames = frames.load();
    s.averageFrameMs = averagePaintMs.load();

    const auto last = lastPaintAtMs.load();
    s.msSinceLastFrame = last >= 0.0 ? clock() - last : -1.0;
    s.lastFailure = lastFailure;
    s.lastFailureDetail = lastFailureDetail;
    return s;
}
} // namespace snds::rendering
