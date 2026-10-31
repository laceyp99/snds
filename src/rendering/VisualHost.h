#pragma once

#include "rendering/RenderOverrides.h"
#include "rendering/VisualRenderer.h"

#include <functional>
#include <memory>
#include <vector>

namespace snds::rendering
{
/** A component a panel can host to show reactive artwork. It owns the current renderer tier and
    walks down the ladder (gl, cpu, static) when a tier cannot start or fails later, recording a
    FallbackEvent for each step. It never touches audio and never throws.

    Message thread only. Call start() once the host is showing; the artwork and effect parameters
    can be set at any time and are re-delivered to whichever tier is current.

    Layout. The GL tier draws into a child component that covers the host (JUCE puts a native
    window over whatever it attaches to), so the other tiers can paint on the host itself and show
    the moment the GL surface is hidden. The host paints nothing for GL; put controls in sibling
    components, not children of this one.

    Pacing. The host's timer runs at the rate the current tier asks for (about 60 Hz only for the
    CPU tier, which paints on this thread; 10 Hz health checks for GL; 2 Hz for static).

    Hung drivers. Detaching an OpenGL context blocks until its render thread finishes its frame,
    which never happens if the driver hung. So a GL renderer that failed is not detached on the spot:
    it is retired. The surface is moved out of view at once and the next tier takes over, and the
    retired renderer is detached later, only once its render thread answers again. (Hiding or
    resizing the surface to nothing would make JUCE detach the context itself, and block.) If the host is destroyed
    first, a renderer that is still hung is leaked on purpose, because a blocked shutdown is worse
    than a leaked driver object.
*/
class VisualHost : public juce::Component, private juce::Timer
{
public:
    /** Makes the renderer for a tier, or nullptr when that tier is not available (the host skips
        it). The default factory builds the real tiers; tests inject fakes. */
    using RendererFactory = std::function<std::unique_ptr<VisualRenderer> (RenderTier tier)>;
    using FallbackListener = std::function<void (const FallbackEvent&)>;

    explicit VisualHost (RenderOverrides overrides = RenderOverrides::fromEnvironment(), RendererFactory factory = {});
    ~VisualHost() override;

    void setArtwork (const ArtworkInput& artwork);
    void setEffects (const EffectParams& effects);

    /** Static mode (Original A/B): every tier shows the plain artwork, whatever effects are set. The
        effects are kept and come back when static mode ends. */
    void setStaticMode (bool shouldBeStatic);
    bool isStaticMode() const noexcept { return staticMode; }

    /** Picks the best tier that starts. Does nothing if already started. */
    void start();

    /** Detaches the current tier. start() can be called again (it begins at the top of the ladder). */
    void stop();

    RenderTier tier() const noexcept { return currentTier; }
    RendererStatus status() const;
    const std::vector<FallbackEvent>& events() const noexcept { return fallbackEvents; }

    /** Called on the message thread after each step down the ladder. */
    void setFallbackListener (FallbackListener listener) { onFallback = std::move (listener); }

    /** One health check and, for the CPU tier, one frame; the host's timer calls this. Public so
        tests can drive it. */
    void tick();

    /** The timer rate the host is running at (0 when stopped). */
    double tickHz() const noexcept { return currentTickHz; }

    /** Renderers that failed and are waiting for a responsive render thread before being detached. */
    int retiredCount() const noexcept { return static_cast<int> (retired.size()); }

    bool isStarted() const noexcept { return started; }

    /** The host is partly or wholly covered or scrolled out of view. A GL surface is a native window
        that nothing can be drawn over or clipped by, so it is moved out of sight (never hidden: that
        would make JUCE detach the context) and the plain artwork shows in its place. */
    void setOccluded (bool shouldBeOccluded);
    bool isOccluded() const noexcept { return occluded; }

    /** True while the GL surface is shown (the GL tier is current). */
    bool isGlSurfaceVisible() const noexcept { return surface->isVisible() && ! surfaceParked; }

    void paint (juce::Graphics& g) override;
    void resized() override;

    /** The tiers from best to worst. */
    static constexpr RenderTier ladder[] = { RenderTier::gl, RenderTier::cpu, RenderTier::staticImage };

    /** Tests only: when false, a renderer that is still hung at destruction is destroyed instead of
        leaked. (A real hung GL context would block the destructor; the default, true, never does.) */
    static void setLeakHungRenderersOnShutdown (bool leak) noexcept;

    /** A render thread that has not been heard from for this long is treated as hung. */
    static constexpr double hungAfterMs = 1000.0;

private:
    void timerCallback() override { tick(); }

    // Starts the first tier at or after `index` in the ladder that attaches, recording each skipped one.
    void enterLadderAt (std::size_t index);
    void onRendererFailure (std::uint64_t generation, FallbackCause cause, const juce::String& detail);
    void record (RenderTier from, RenderTier to, FallbackCause cause, const juce::String& detail);
    void retireCurrentRenderer();
    void pollRetired();
    void updateTimer();
    void parkSurface();
    static bool isHung (const RendererStatus& status) noexcept;

    EffectParams effectiveEffects() const noexcept { return staticMode ? EffectParams() : effects; }

    struct Surface final : juce::Component
    {
        Surface() { setInterceptsMouseClicks (false, false); }
        void paint (juce::Graphics&) override {} // the GL context draws here, never JUCE
    };

    RenderOverrides overrides;
    RendererFactory factory;
    std::unique_ptr<Surface> surface = std::make_unique<Surface>();
    std::unique_ptr<VisualRenderer> renderer;
    std::vector<std::unique_ptr<VisualRenderer>> retired;
    RenderTier currentTier = RenderTier::none;
    std::size_t ladderIndex = 0;
    bool occluded = false;
    bool surfaceParked = false; // moved off-screen (not hidden) while a hung renderer is attached to it
    bool started = false;
    double currentTickHz = 0.0;
    std::uint64_t generation = 0; // identifies the renderer a deferred failure belongs to

    ArtworkInput artwork;
    EffectParams effects;
    bool staticMode = false;
    std::vector<FallbackEvent> fallbackEvents;
    FallbackListener onFallback;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VisualHost)
};
} // namespace snds::rendering
