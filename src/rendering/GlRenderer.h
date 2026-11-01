#pragma once

#include "rendering/RenderOverrides.h"
#include "rendering/VisualRenderer.h"

#include <memory>

namespace snds::rendering
{
/** The GPU tier: a GLSL 1.50 fragment shader drawn through juce::OpenGLContext on JUCE's render
    thread. No OpenGL type appears in this header; everything lives in the implementation.

    What the spike (spikes/opengl-tier) established, and this follows:
      - OpenGL 3.2 core is requested, but the version and GLSL version the driver reports after
        the context exists are what is checked (a failed request silently becomes a legacy context).
      - Component painting is off for the attached component, so the render thread never takes the
        message-manager lock. The component must paint nothing itself; put UI in sibling components.
      - attach() blocks the message thread (about 30 to 240 ms); detach() blocks until the render
        thread finishes its frame, so both are called once and only from the message thread.
      - Release builds have a no-error GL context: failures are found through the capability check,
        the shader log, resource setup errors and the frame watchdog, not glGetError.
      - The render thread never touches message-thread state. It reports through atomics and one
        callAsync, and only the message thread detaches the context.

    The host component must be showing (have a native peer) for the context to be created; if it
    is not yet, creation happens when it appears and tick() reports a failure if it never does.
*/
class GlRenderer final : public VisualRenderer
{
public:
    explicit GlRenderer (RenderOverrides overrides = {});
    ~GlRenderer() override;

    RenderTier tier() const noexcept override { return RenderTier::gl; }

    AttachResult attach (juce::Component& host) override;
    void detach() override;

    void setArtwork (const ArtworkInput& artwork) override;
    void setEffects (const EffectParams& effects) override;

    void paint (juce::Graphics&) override {} // drawn by the GL context
    void tick() override;

    RendererStatus status() const override;
    void setFailureCallback (FailureCallback callback) override;

    /** The watchdog: no frame for this long while the host is visible is a failure. */
    static constexpr double watchdogMs = 2000.0;

    double preferredTickHz() const noexcept override { return pacing::glHealthHz; }

private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};
} // namespace snds::rendering
