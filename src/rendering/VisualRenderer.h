#pragma once

#include "rendering/FramePacing.h"
#include "rendering/RenderTypes.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace snds::rendering
{
/** A tier of the artwork renderer. Free of OpenGL (and any other graphics API) types, so the host,
    the mappings and the tests never depend on how a tier draws.

    Lifecycle, all on the message thread unless noted:
      attach(host)   start drawing into a component. Can block (GL: 30 to 240 ms to create a
                     context), so call it once, when the visuals open.
      setArtwork /   hand over data. Cheap; the latest value wins. Safe to call before attach.
      setEffects
      paint(g)       called from the host's paint(). Tiers that draw elsewhere (GL) do nothing.
      tick()         once per UI timer tick: health checks (watchdog) and per-frame work.
      detach()       stop and release everything. Can block until the render thread finishes its
                     frame, so it is called once per tier, from the message thread, never from
                     the render thread.
    A renderer that fails after attach reports through the failure callback, on the message
    thread, exactly once, and then draws nothing more; the host decides what comes next.
*/
class VisualRenderer
{
public:
    struct AttachResult
    {
        bool ok = true;
        FallbackCause cause = FallbackCause::none;
        juce::String detail;
    };

    /** A failure that happened after attach (a lost context, a watchdog, ...). */
    using FailureCallback = std::function<void (FallbackCause cause, const juce::String& detail)>;

    virtual ~VisualRenderer() = default;

    virtual RenderTier tier() const noexcept = 0;

    virtual AttachResult attach (juce::Component& host) = 0;
    virtual void detach() = 0;

    virtual void setArtwork (const ArtworkInput& artwork) = 0;
    virtual void setEffects (const EffectParams& effects) = 0;

    virtual void paint (juce::Graphics& g) = 0;
    virtual void tick() = 0;

    /** Thread-safe: callable from the message thread at any time. */
    virtual RendererStatus status() const = 0;

    /** Set before attach. Invoked on the message thread. */
    virtual void setFailureCallback (FailureCallback callback) = 0;

    /** How often the host should call tick() (and, for a tier that paints on the message thread,
        repaint). Re-read after every tick, so a tier can slow itself down. */
    virtual double preferredTickHz() const noexcept { return pacing::staticHealthHz; }
};
} // namespace snds::rendering
