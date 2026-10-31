#pragma once

#include <juce_opengl/juce_opengl.h>

#include <atomic>
#include <memory>
#include <vector>

#include "Options.h"

namespace snds::spike
{
enum class Tier
{
    none,
    gl,
    cpu,
    staticImage
};

juce::String tierName (Tier);

// Milliseconds since the first call (made at app start) on a high-resolution clock.
double nowMs() noexcept;

// Deterministic 120 BPM beat envelope standing in for an analysis snapshot.
float pulseAt (double seconds) noexcept;

struct GlInfo
{
    bool contextCreated = false;
    juce::String vendor, renderer, version, glsl, profile;
    int major = 0, minor = 0, contextFlags = 0, maxTextureSize = 0;
    double glslVersion = 0.0;
    bool meetsMinimum = false;
    bool shaderCompiled = false;
    juce::String shaderLog;
    double initMs = 0.0;
};

struct Transition
{
    Tier from = Tier::none, to = Tier::none;
    juce::String reason;
    double atMs = 0.0, detachMs = 0.0, completedAtMs = 0.0;
};

struct FrameCapture
{
    double atSeconds = 0.0, pulse = 0.0, meanLuma = 0.0;
};

// Shows a generated artwork image with a pulse effect. Tiers, best first:
//   gl          GLSL fragment shader through juce::OpenGLContext (render thread)
//   cpu         juce::Graphics paint() driven by the message-thread timer
//   staticImage the image without animation
// The GL thread never blocks the message thread: it reports through atomics and
// MessageManager::callAsync, and only the message thread detaches the context.
class ArtworkView final : public juce::Component,
                          private juce::OpenGLRenderer
{
public:
    explicit ArtworkView (const Options&);
    ~ArtworkView() override;

    // Message thread only.
    void startVisuals();
    void onTimerTick();
    void paint (juce::Graphics&) override;

    Tier getTier() const noexcept { return tier; }
    bool wasGlAttempted() const noexcept { return glAttempted; }
    double getAttachStartMs() const noexcept { return attachStartMs; }
    double getAttachCallMs() const noexcept { return attachCallMs; }
    const GlInfo& getGlInfo() const noexcept { return glInfo; }
    const std::vector<Transition>& getTransitions() const noexcept { return transitions; }
    const std::vector<FrameCapture>& getCaptures() const noexcept { return captures; }
    const juce::Image& getGlCaptureImage() const noexcept { return glCaptureImage; }
    juce::int64 getGlFrames() const noexcept { return glFrames.load(); }
    double getGlFirstFrameMs() const noexcept { return glFirstFrameMs.load(); }
    double getGlLastFrameMs() const noexcept { return glLastFrameMs.load(); }
    double getGlMaxFrameGapMs() const noexcept { return glMaxFrameGapMs.load(); }
    int getGlErrorCount() const noexcept { return glErrorCount.load(); }
    int getCpuFrames() const noexcept { return cpuFrames; }
    double getCpuPaintTotalMs() const noexcept { return cpuPaintTotalMs; }
    double getCpuPaintMaxMs() const noexcept { return cpuPaintMaxMs; }

    static constexpr double glWatchdogMs = 2000.0;
    static constexpr double cpuBudgetMs = 8.0;

private:
    // OpenGLRenderer. newOpenGLContextCreated/renderOpenGL run on JUCE's render
    // thread; openGLContextClosing runs on whichever thread detaches (here the
    // message thread, inside OpenGLContext::detach()).
    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;

    void postGlFailure (const juce::String& reason);                 // any thread
    void fallBack (Tier to, const juce::String& reason);             // message thread
    void paintArtwork (juce::Graphics&, float pulse, bool animated); // message thread
    void captureFrame (double seconds, float pulse);                 // render thread

    const Options options;
    juce::Image artworkSoftware; // immutable after construction; read by the render thread
    juce::Image artworkNative;   // message-thread copy for juce::Graphics

    // Message-thread state.
    Tier tier = Tier::none;
    bool glAttempted = false;
    double attachStartMs = -1.0, attachCallMs = 0.0;
    GlInfo glInfo;
    std::vector<Transition> transitions;
    std::vector<FrameCapture> captures;
    juce::Image glCaptureImage;
    int cpuFrames = 0;
    double cpuPaintTotalMs = 0.0, cpuPaintMaxMs = 0.0;
    std::vector<double> recentCpuPaintMs;

    // Created on the message thread so the render thread only ever copies it
    // (first SafePointer construction allocates the shared weak reference).
    const juce::Component::SafePointer<ArtworkView> selfRef { this };

    // Shared between threads.
    std::atomic<bool> glReady { false };
    std::atomic<bool> glFailurePosted { false };
    std::atomic<juce::int64> glFrames { 0 };
    std::atomic<double> glFirstFrameMs { -1.0 };
    std::atomic<double> glLastFrameMs { -1.0 };
    std::atomic<double> glMaxFrameGapMs { 0.0 };
    std::atomic<int> glErrorCount { 0 };

    // Render-thread state (also released in openGLContextClosing after the
    // render thread has stopped).
    juce::OpenGLContext glContext;
    std::unique_ptr<juce::OpenGLShaderProgram> shader;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> uArtwork, uPulse, uAspect;
    juce::OpenGLTexture texture;
    juce::uint32 vao = 0, vbo = 0;
    int nextCaptureIndex = 0;
    bool glHangDone = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArtworkView)
};
} // namespace snds::spike
