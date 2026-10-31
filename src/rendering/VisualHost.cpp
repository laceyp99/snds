#include "rendering/VisualHost.h"

#include "rendering/CpuRenderer.h"
#include "rendering/GlRenderer.h"
#include "rendering/StaticRenderer.h"

#include <cmath>

namespace snds::rendering
{
namespace
{
std::unique_ptr<VisualRenderer> makeDefaultRenderer (RenderTier tier, const RenderOverrides& overrides)
{
    switch (tier)
    {
        case RenderTier::gl:
            return std::make_unique<GlRenderer> (overrides);
        case RenderTier::cpu:
            return std::make_unique<CpuRenderer> (overrides);
        case RenderTier::staticImage:
            return std::make_unique<StaticRenderer>();
        case RenderTier::none:
            break;
    }

    return nullptr;
}
bool leakHungOnShutdown = true;
} // namespace

void VisualHost::setLeakHungRenderersOnShutdown (bool leak) noexcept
{
    leakHungOnShutdown = leak;
}

VisualHost::VisualHost (RenderOverrides o, RendererFactory f)
    : overrides (std::move (o)),
      factory (std::move (f))
{
    if (! factory)
        factory = [this] (RenderTier tier) { return makeDefaultRenderer (tier, overrides); };

    setOpaque (true);
    addChildComponent (*surface);
}

VisualHost::~VisualHost()
{
    stopTimer();
    ++generation; // deferred failures from the dying renderers must not call back into us

    // A renderer whose render thread is hung cannot be detached without blocking the shutdown, and
    // neither can the component it is attached to be destroyed. Leak them rather than hang.
    bool leaked = false;

    auto release = [&leaked] (std::unique_ptr<VisualRenderer>& r)
    {
        if (r == nullptr)
            return;

        if (isHung (r->status()) && leakHungOnShutdown)
        {
            juce::Logger::writeToLog ("visuals: leaking a " + tierName (r->tier())
                                      + " renderer whose render thread is hung (shutdown must not block)");
            static_cast<void> (r.release());
            leaked = true;
        }
        else
        {
            if (! isHung (r->status()))
                r->detach();

            r.reset();
        }
    };

    release (renderer);

    for (auto& r : retired)
        release (r);

    if (leaked)
    {
        // The hung context is still attached to the surface: destroying the surface would block too.
        removeChildComponent (surface.get());
        static_cast<void> (surface.release());
    }
}

void VisualHost::setArtwork (const ArtworkInput& newArtwork)
{
    artwork = newArtwork;

    if (renderer != nullptr)
        renderer->setArtwork (artwork);
}

void VisualHost::setEffects (const EffectParams& newEffects)
{
    effects = newEffects;

    if (renderer != nullptr)
        renderer->setEffects (effectiveEffects());
}

void VisualHost::setStaticMode (bool shouldBeStatic)
{
    staticMode = shouldBeStatic;

    if (renderer != nullptr)
        renderer->setEffects (effectiveEffects());
}

RendererStatus VisualHost::status() const
{
    if (renderer != nullptr)
        return renderer->status();

    return {};
}

bool VisualHost::isHung (const RendererStatus& status) noexcept
{
    return status.attached && status.msSinceHeartbeat > hungAfterMs;
}

void VisualHost::record (RenderTier from, RenderTier to, FallbackCause cause, const juce::String& detail)
{
    FallbackEvent event;
    event.from = from;
    event.to = to;
    event.cause = cause;
    event.detail = detail;
    event.atMs = renderClockMs();
    fallbackEvents.push_back (event);

    juce::Logger::writeToLog ("visuals: " + tierName (from) + " -> " + tierName (to) + " (" + causeName (cause)
                              + "): " + detail);

    if (onFallback)
        onFallback (event);
}

void VisualHost::resized()
{
    if (! surfaceParked)
        surface->setBounds (getLocalBounds());
}

void VisualHost::parkSurface()
{
    // Off to the side, still visible and the same size: JUCE only repositions the native window.
    surfaceParked = true;
    surface->setBounds (-20000, -20000, std::max (1, getWidth()), std::max (1, getHeight()));
}

void VisualHost::start()
{
    if (started)
        return;

    started = true;
    enterLadderAt (0);
    updateTimer();
}

void VisualHost::stop()
{
    ++generation;
    started = false;
    retireCurrentRenderer();
    currentTier = RenderTier::none;
    updateTimer(); // keeps ticking only if a retired renderer is still waiting to be detached
}

void VisualHost::enterLadderAt (std::size_t index)
{
    const auto count = std::size (ladder);

    for (auto i = index; i < count; ++i)
    {
        const auto candidate = ladder[i];
        const auto next = i + 1 < count ? ladder[i + 1] : candidate;
        auto fresh = factory (candidate);

        if (fresh == nullptr)
        {
            // Not usable here: step over it. (The last tier is always available.)
            if (i + 1 < count)
                record (candidate,
                        next,
                        FallbackCause::unavailable,
                        "the " + tierName (candidate) + " tier is not available");
            continue;
        }

        const auto thisGeneration = ++generation;
        fresh->setFailureCallback ([this, thisGeneration] (FallbackCause failure, const juce::String& message)
                                   { onRendererFailure (thisGeneration, failure, message); });
        fresh->setArtwork (artwork);
        fresh->setEffects (effectiveEffects());

        const auto onSurface = candidate == RenderTier::gl;

        if (onSurface && ! retired.empty())
        {
            // A GL renderer is still hung on the surface: do not attach a second context to it.
            if (i + 1 < count)
                record (candidate,
                        next,
                        FallbackCause::unavailable,
                        "the previous OpenGL renderer has not released the surface yet");
            continue;
        }

        if (onSurface)
        {
            surfaceParked = false;
            surface->setBounds (getLocalBounds());
            surface->setVisible (true);
        }

        const auto result = fresh->attach (onSurface ? static_cast<juce::Component&> (*surface) : *this);

        if (! result.ok)
        {
            ++generation;
            surface->setVisible (false);
            fresh->detach();

            if (i + 1 < count)
                record (candidate, next, result.cause, result.detail);
            continue;
        }

        renderer = std::move (fresh);
        currentTier = candidate;
        ladderIndex = i;
        repaint();
        return;
    }

    // Unreachable with the default factory (the static tier cannot fail); a test factory may refuse everything.
    currentTier = RenderTier::none;
}

void VisualHost::retireCurrentRenderer()
{
    if (renderer == nullptr)
        return;

    auto old = std::move (renderer);

    if (isHung (old->status()))
    {
        // Do not detach: it would block this thread until the driver answers, if ever. Move the
        // surface out of view so the next tier shows through, and detach once the thread responds.
        parkSurface();
        juce::Logger::writeToLog ("visuals: the " + tierName (old->tier())
                                  + " render thread is not responding; detaching it later");
        retired.push_back (std::move (old));
    }
    else
    {
        old->detach(); // the render thread is alive, so this takes a frame or two
        surface->setVisible (false);
    }
}

void VisualHost::pollRetired()
{
    for (auto it = retired.begin(); it != retired.end();)
    {
        if (isHung ((*it)->status()))
        {
            ++it;
            continue;
        }

        (*it)->detach(); // the thread is answering again: this is quick
        it = retired.erase (it);

        if (retired.empty() && renderer != nullptr && currentTier != RenderTier::gl)
        {
            surface->setVisible (false);
            surfaceParked = false;
        }
    }
}

void VisualHost::onRendererFailure (std::uint64_t failedGeneration, FallbackCause cause, const juce::String& detail)
{
    // Called by a renderer, possibly from inside its own tick(): swap on a later message.
    juce::MessageManager::callAsync (
        [safe = juce::Component::SafePointer<VisualHost> (this), failedGeneration, cause, detail]
        {
            if (safe == nullptr || safe->generation != failedGeneration || safe->renderer == nullptr)
                return;

            auto* self = safe.getComponent();
            const auto failedTier = self->currentTier;
            const auto failedIndex = self->ladderIndex;

            ++self->generation; // the failed renderer's later reports are stale
            self->retireCurrentRenderer();
            self->currentTier = RenderTier::none;

            const auto next = failedIndex + 1 < std::size (ladder) ? ladder[failedIndex + 1] : failedTier;
            self->record (failedTier, next, cause, detail);
            self->enterLadderAt (failedIndex + 1);
            self->updateTimer();
        });
}

void VisualHost::updateTimer()
{
    auto wanted = renderer != nullptr ? renderer->preferredTickHz() : 0.0;

    if (! retired.empty())
        wanted = std::max (wanted, pacing::glHealthHz); // keep polling until they are detached

    if (wanted == currentTickHz)
        return;

    currentTickHz = wanted;

    if (wanted <= 0.0)
        stopTimer();
    else
        startTimerHz (std::max (1, static_cast<int> (std::lround (wanted))));
}

void VisualHost::tick()
{
    pollRetired();

    if (renderer != nullptr)
    {
        renderer->tick();

        if (currentTier == RenderTier::cpu)
            repaint(); // the only tier that is drawn frame by frame on this thread
    }

    updateTimer();
}

void VisualHost::setOccluded (bool shouldBeOccluded)
{
    if (occluded == shouldBeOccluded)
        return;

    occluded = shouldBeOccluded;

    if (renderer == nullptr || currentTier != RenderTier::gl)
        return;

    if (occluded)
    {
        parkSurface();
    }
    else if (retired.empty())
    {
        surfaceParked = false;
        surface->setBounds (getLocalBounds());
    }

    repaint();
}

void VisualHost::paint (juce::Graphics& g)
{
    if (renderer != nullptr && currentTier != RenderTier::gl)
    {
        renderer->paint (g);
        return;
    }

    g.fillAll (juce::Colours::black);

    // The GL surface is out of view (occluded): show the plain artwork where it would have been.
    if (surfaceParked && artwork.isValid())
    {
        const auto area = StaticRenderer::fitRectangle (
            getLocalBounds().toFloat(), artwork.image.getWidth(), artwork.image.getHeight());
        g.drawImage (artwork.image, area, juce::RectanglePlacement::stretchToFit);
    }
}
} // namespace snds::rendering
