#include "rendering/GlRenderer.h"

#include "rendering/FramePacing.h"
#include "rendering/GlCapability.h"

#include <juce_opengl/juce_opengl.h>

#include <array>
#include <atomic>
#include <mutex>

namespace snds::rendering
{
namespace
{
// GLSL 1.50 is the shading language of OpenGL 3.2 core: the minimum tier.
constexpr const char* vertexShaderSource = R"(#version 150
in vec2 position;
out vec2 uv;

void main()
{
    uv = position * 0.5 + 0.5;
    gl_Position = vec4 (position, 0.0, 1.0);
}
)";

// Deterministic: nothing here depends on time, only on the uniforms and the pixel position.
constexpr const char* fragmentShaderSource = R"(#version 150
in vec2 uv;
out vec4 fragColour;

uniform sampler2D artwork;
uniform vec2 fit;             // 1 on the axis the artwork fills, above 1 where it is letterboxed
uniform float scale;          // zoom about the centre
uniform float brightness;
uniform float depth;          // slow push-in and darker edges
uniform float glow;
uniform float grain;
uniform vec2 sideBrightness;  // extra brightness towards the left and the right edge
uniform float shift;          // horizontal drift
uniform vec3 glowTint;

float hash (vec2 p)
{
    return fract (sin (dot (p, vec2 (12.9898, 78.233))) * 43758.5453);
}

void main()
{
    vec2 p = (uv - 0.5) * fit;
    p.x -= shift * 0.015;
    vec2 st = p / (scale * (1.0 + 0.03 * depth)) + 0.5;

    if (any (lessThan (st, vec2 (0.0))) || any (greaterThan (st, vec2 (1.0))))
    {
        fragColour = vec4 (0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 base = texture (artwork, st).rgb;
    vec3 colour = base;

    if (glow > 0.0)
    {
        vec3 bloom = vec3 (0.0);
        float radius = 0.004 + 0.02 * glow;

        for (int i = 0; i < 8; ++i)
        {
            float a = 6.2831853 * float (i) / 8.0;
            bloom += texture (artwork, st + radius * vec2 (cos (a), sin (a))).rgb;
        }

        bloom /= 8.0;
        colour = mix (base, max (base, bloom) * 1.3 + glowTint * 0.15 * glow, 0.5 * glow);
    }

    colour *= 1.0 - 0.35 * depth * smoothstep (0.25, 0.75, length (st - 0.5));

    float side = sideBrightness.x * (1.0 - uv.x) * (1.0 - uv.x) + sideBrightness.y * uv.x * uv.x;
    colour *= 1.0 + 0.6 * side;

    if (grain > 0.0)
        colour += (hash (floor (st * 512.0)) - 0.5) * 0.12 * grain;

    fragColour = vec4 (clamp (colour * brightness, 0.0, 1.0), 1.0);
}
)";

// A deliberate syntax error (missing semicolon) for the shader-failure switch.
constexpr const char* brokenFragmentShaderSource = R"(#version 150
out vec4 fragColour;

void main()
{
    fragColour = vec4 (1.0, 0.0, 1.0, 1.0)
}
)";

juce::String glString (GLenum name)
{
    const auto* s = juce::gl::glGetString (name);
    return s != nullptr ? juce::String::fromUTF8 (reinterpret_cast<const char*> (s)) : juce::String();
}

constexpr std::size_t effectFieldCount = 8;
} // namespace

//==============================================================================
struct GlRenderer::Impl final : public std::enable_shared_from_this<Impl>, private juce::OpenGLRenderer
{
    explicit Impl (RenderOverrides o)
        : overrides (std::move (o))
    {
        storeEffects (EffectParams());
    }

    ~Impl() override { detach(); }

    //==========================================================================
    // Message thread.

    AttachResult attach (juce::Component& newHost)
    {
        if (overrides.forceNoOpenGL)
            return { false, FallbackCause::disabledByRequest, "OpenGL skipped (forced by the render overrides)" };

        host = &newHost;
        context.setRenderer (this);
        // Without component painting JUCE's render thread never takes the message-manager lock, so
        // GL frames cannot stall message-thread timers (spike finding 3).
        context.setComponentPaintingEnabled (false);
        context.setContinuousRepainting (true);
        context.setOpenGLVersionRequired (juce::OpenGLContext::openGL3_2);

        // About 60 fps: the swap interval follows the display's refresh rate (applied once the
        // context is current, on the render thread).
        const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (newHost.getScreenBounds());
        const auto refresh = display != nullptr ? display->verticalFrequencyHz.value_or (60.0) : 60.0;
        swapInterval = pacing::swapIntervalFor (refresh);

        attachedAtMs = renderClockMs();
        context.attachTo (newHost); // blocks while the native context is created, if the host is showing
        const auto attachMs = renderClockMs() - attachedAtMs;
        wasAttachRequested = true;

        if (! context.isAttached() && newHost.isShowing())
        {
            host = nullptr;
            return { false, FallbackCause::contextCreationFailed, "the native OpenGL context could not be created" };
        }

        juce::ignoreUnused (attachMs);
        return {};
    }

    void detach()
    {
        ready = false;

        if (wasAttachRequested)
        {
            // Blocks until the render thread finishes its frame; openGLContextClosing() runs here.
            context.detach();
            wasAttachRequested = false;
        }

        host = nullptr;
    }

    void setArtwork (const ArtworkInput& newArtwork)
    {
        ArtworkInput converted;
        converted.palette = newArtwork.palette;

        if (newArtwork.image.isValid())
            converted.image = juce::SoftwareImageType().convert (newArtwork.image); // readable from the render thread

        {
            const std::lock_guard lock (artworkMutex);
            artwork = std::move (converted);
            ++artworkGeneration;
        }
    }

    void setEffects (const EffectParams& effects) { storeEffects (effects); }

    void tick()
    {
        if (failurePosted.load() || host == nullptr)
            return;

        const auto visible = host->isShowing() && host->getPeer() != nullptr && ! host->getPeer()->isMinimised();

        if (! context.isAttached())
        {
            // The host was not showing at attach(): JUCE creates the context when it appears.
            if (visible && renderClockMs() - attachedAtMs > watchdogMs)
                fail (FallbackCause::contextCreationFailed, "the OpenGL context was never created");
            return;
        }

        // No frames are wanted while the window is hidden or minimised: stop the render loop instead
        // of letting it spin, and give the watchdog a fresh start when it comes back.
        if (visible != repainting)
        {
            repainting = visible;
            context.setContinuousRepainting (visible);

            if (visible)
                resumedAtMs = renderClockMs();
        }

        const auto reference = juce::jmax (lastFrameMs.load(), attachedAtMs, resumedAtMs);

        if (visible && renderClockMs() - reference > watchdogMs)
            fail (FallbackCause::watchdogTimeout,
                  "no OpenGL frame for " + juce::String (juce::roundToInt (watchdogMs))
                      + " ms (render thread stalled or context lost)");
    }

    RendererStatus status() const
    {
        RendererStatus s;
        s.tier = RenderTier::gl;
        s.attached = context.isAttached();
        s.ready = ready.load();
        s.frames = frames.load();

        const auto last = lastFrameMs.load();
        s.msSinceLastFrame = last >= 0.0 ? renderClockMs() - last : -1.0;
        s.maxFrameGapMs = maxFrameGapMs.load();
        s.averageFrameMs = averageFrameMs.load();

        const auto beat = heartbeatMs.load();
        s.msSinceHeartbeat = beat >= 0.0 ? renderClockMs() - beat : -1.0;
        s.glErrors = glErrors.load();

        const std::lock_guard lock (infoMutex);
        s.gpu = gpu;
        s.lastFailure = lastFailure;
        s.lastFailureDetail = lastFailureDetail;
        return s;
    }

    void setFailureCallback (FailureCallback callback) { onFailure = std::move (callback); }

    //==========================================================================
    // Any thread: record a failure and tell the message thread, once.
    void fail (FallbackCause cause, const juce::String& detail)
    {
        ready = false;

        {
            const std::lock_guard lock (infoMutex);
            if (lastFailure == FallbackCause::none)
            {
                lastFailure = cause;
                lastFailureDetail = detail;
            }
        }

        if (failurePosted.exchange (true))
            return;

        juce::MessageManager::callAsync (
            [weak = weak_from_this(), cause, detail]
            {
                if (const auto self = weak.lock())
                    if (self->onFailure)
                        self->onFailure (cause, detail);
            });
    }

    //==========================================================================
    // Render thread (and the message thread inside detach()).

    void newOpenGLContextCreated() override
    {
        using namespace juce::gl;

        const auto start = renderClockMs();
        GpuInfo info;
        info.contextCreated = true;
        info.vendor = glString (GL_VENDOR);
        info.renderer = glString (GL_RENDERER);
        info.version = glString (GL_VERSION);
        info.glslVersion = glString (GL_SHADING_LANGUAGE_VERSION);
        info.glslNumber = parseGlslVersion (info.glslVersion);

        while (glGetError() != GL_NO_ERROR)
        {
        }

        GLint major = 0, minor = 0;
        glGetIntegerv (GL_MAJOR_VERSION, &major);
        glGetIntegerv (GL_MINOR_VERSION, &minor);

        if (glGetError() != GL_NO_ERROR || major <= 0)
        {
            // Pre-3.0 contexts (for example "GDI Generic" 1.1) have no GL_MAJOR_VERSION.
            const auto parsed = parseGlVersion (info.version);
            major = parsed ? parsed->first : 0;
            minor = parsed ? parsed->second : 0;
        }

        info.major = static_cast<int> (major);
        info.minor = static_cast<int> (minor);
        const auto glVersion = info.major * 10 + info.minor;

        if (glVersion >= 30)
        {
            GLint flags = 0;
            glGetIntegerv (GL_CONTEXT_FLAGS, &flags);
            info.contextFlags = static_cast<int> (flags);
        }

        if (glVersion >= 32)
        {
            GLint mask = 0;
            glGetIntegerv (GL_CONTEXT_PROFILE_MASK, &mask);
            info.profile = (mask & GL_CONTEXT_CORE_PROFILE_BIT) != 0          ? "core"
                         : (mask & GL_CONTEXT_COMPATIBILITY_PROFILE_BIT) != 0 ? "compatibility"
                                                                              : "unknown";
        }
        else
        {
            info.profile = "legacy";
        }

        GLint maxTexture = 0;
        glGetIntegerv (GL_MAX_TEXTURE_SIZE, &maxTexture);
        info.maxTextureSize = static_cast<int> (maxTexture);

        evaluateCapability (info, overrides.requirement);
        context.setSwapInterval (swapInterval.load());
        maxTextureSize = info.maxTextureSize;

        FallbackCause cause = FallbackCause::none;
        juce::String detail;

        if (! info.meetsMinimum)
        {
            cause = FallbackCause::belowMinimumVersion;
            detail = "capability: " + info.verdict;
        }
        else
        {
            shader = std::make_unique<juce::OpenGLShaderProgram> (context);
            const auto* fragment = overrides.forceShaderFailure ? brokenFragmentShaderSource : fragmentShaderSource;

            if (shader->addVertexShader (vertexShaderSource) && shader->addFragmentShader (fragment) && shader->link())
            {
                bindUniforms();
                createQuad();

                if (const auto error = glGetError(); error != GL_NO_ERROR)
                {
                    cause = FallbackCause::resourceSetupFailed;
                    detail = "OpenGL error 0x" + juce::String::toHexString (static_cast<int> (error))
                           + " during resource setup";
                }
            }
            else
            {
                cause = FallbackCause::shaderCompileFailed;
                detail = "GLSL compile or link failed: " + shader->getLastError().trim();
                shader.reset();
            }
        }

        info.initMs = renderClockMs() - start;

        {
            const std::lock_guard lock (infoMutex);
            gpu = info;
        }

        if (cause != FallbackCause::none)
            fail (cause, detail);
        else
            ready = true;
    }

    void renderOpenGL() override
    {
        using namespace juce::gl;

        const auto frameStart = renderClockMs();
        heartbeatMs = frameStart;

        // However the frame ends (even early, on a failure), never run faster than the floor: a driver
        // that ignores the swap interval must not turn the render thread into a busy loop.
        const struct Pace
        {
            ~Pace()
            {
                const auto spare = pacing::minFrameIntervalMs() - (renderClockMs() - start);

                if (spare >= 1.0)
                    juce::Thread::sleep (static_cast<int> (spare));
            }

            double start;
        } pace { frameStart };

        glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
        glClear (GL_COLOR_BUFFER_BIT);

        if (! ready.load())
            return;

        const auto now = frameStart;
        auto first = firstFrameMs.load();

        if (first < 0.0)
        {
            first = now;
            firstFrameMs = now;
        }

        if (overrides.forceContextLoss && now - first >= overrides.contextLossAfterMs)
        {
            fail (FallbackCause::contextLost,
                  "simulated context loss after " + juce::String (juce::roundToInt (overrides.contextLossAfterMs))
                      + " ms of rendering (forced by the render overrides)");
            return;
        }

        // Diagnostic: simulate a driver hang by blocking the render thread once.
        if (overrides.forceGlHang && ! hangDone && now - first >= 1500.0)
        {
            hangDone = true;
            juce::Thread::sleep (static_cast<int> (overrides.glHangMs));
        }

        uploadArtworkIfChanged();

        GLint viewport[4] = {};
        glGetIntegerv (GL_VIEWPORT, viewport);
        const auto viewW = static_cast<float> (juce::jmax (1, static_cast<int> (viewport[2])));
        const auto viewH = static_cast<float> (juce::jmax (1, static_cast<int> (viewport[3])));

        const auto effects = loadEffects();
        const auto viewAspect = viewW / viewH;
        const auto fitX = juce::jmax (1.0f, viewAspect / artworkAspect);
        const auto fitY = juce::jmax (1.0f, artworkAspect / viewAspect);

        shader->use();
        glActiveTexture (GL_TEXTURE0);
        texture.bind();
        uArtwork->set (static_cast<GLint> (0));
        uFit->set (fitX, fitY);
        uScale->set (effects.scale);
        uBrightness->set (effects.brightness);
        uDepth->set (effects.depth);
        uGlow->set (effects.glow);
        uGrain->set (effects.texture);
        uSide->set (effects.leftBrightness, effects.rightBrightness);
        uShift->set (effects.shift);
        uTint->set (glowTint.getFloatRed(), glowTint.getFloatGreen(), glowTint.getFloatBlue());

        glBindVertexArray (vao);
        glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray (0);
        texture.unbind();

        // Release contexts are KHR_no_error (spike finding 2), so this only reports in Debug builds.
        if (const auto error = glGetError(); error != GL_NO_ERROR)
        {
            ++glErrors;

            if (error == GL_CONTEXT_LOST || error == GL_OUT_OF_MEMORY)
            {
                fail (FallbackCause::contextLost,
                      "OpenGL error 0x" + juce::String::toHexString (static_cast<int> (error)) + " while rendering");
                return;
            }
        }

        if (const auto previous = lastFrameMs.load(); previous >= 0.0)
        {
            const auto gap = now - previous;
            maxFrameGapMs = juce::jmax (maxFrameGapMs.load(), gap);
            averageFrameMs = averageFrameMs.load() == 0.0 ? gap : averageFrameMs.load() * 0.9 + gap * 0.1;
        }

        ++frames;
        lastFrameMs = now;
    }

    void openGLContextClosing() override
    {
        using namespace juce::gl;

        ready = false;

        if (vbo != 0)
            glDeleteBuffers (1, &vbo);

        if (vao != 0)
            glDeleteVertexArrays (1, &vao);

        vbo = vao = 0;
        texture.release();
        uArtwork.reset();
        uFit.reset();
        uScale.reset();
        uBrightness.reset();
        uDepth.reset();
        uGlow.reset();
        uGrain.reset();
        uSide.reset();
        uShift.reset();
        uTint.reset();
        shader.reset();
        uploadedGeneration = 0;
    }

    //==========================================================================
    void bindUniforms()
    {
        using Uniform = juce::OpenGLShaderProgram::Uniform;
        uArtwork = std::make_unique<Uniform> (*shader, "artwork");
        uFit = std::make_unique<Uniform> (*shader, "fit");
        uScale = std::make_unique<Uniform> (*shader, "scale");
        uBrightness = std::make_unique<Uniform> (*shader, "brightness");
        uDepth = std::make_unique<Uniform> (*shader, "depth");
        uGlow = std::make_unique<Uniform> (*shader, "glow");
        uGrain = std::make_unique<Uniform> (*shader, "grain");
        uSide = std::make_unique<Uniform> (*shader, "sideBrightness");
        uShift = std::make_unique<Uniform> (*shader, "shift");
        uTint = std::make_unique<Uniform> (*shader, "glowTint");
    }

    void createQuad()
    {
        using namespace juce::gl;

        const GLfloat quad[] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
        glGenVertexArrays (1, &vao);
        glBindVertexArray (vao);
        glGenBuffers (1, &vbo);
        glBindBuffer (GL_ARRAY_BUFFER, vbo);
        glBufferData (GL_ARRAY_BUFFER, sizeof (quad), quad, GL_STATIC_DRAW);
        const auto position = glGetAttribLocation (shader->getProgramID(), "position");
        glEnableVertexAttribArray (static_cast<GLuint> (position));
        glVertexAttribPointer (static_cast<GLuint> (position), 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindVertexArray (0);
        glBindBuffer (GL_ARRAY_BUFFER, 0);
    }

    void uploadArtworkIfChanged()
    {
        std::uint64_t generation = 0;
        ArtworkInput snapshot;

        {
            const std::unique_lock lock (artworkMutex, std::try_to_lock);

            if (! lock.owns_lock())
                return; // the message thread is handing over new art: next frame

            generation = artworkGeneration;

            if (generation == uploadedGeneration)
                return;

            snapshot = artwork;
        }

        uploadedGeneration = generation;

        if (! snapshot.isValid())
        {
            artworkAspect = 1.0f;
            return;
        }

        auto image = snapshot.image;

        if (maxTextureSize > 0 && juce::jmax (image.getWidth(), image.getHeight()) > maxTextureSize)
        {
            const auto factor = static_cast<float> (maxTextureSize)
                              / static_cast<float> (juce::jmax (image.getWidth(), image.getHeight()));
            image = image.rescaled (juce::jmax (1, static_cast<int> (static_cast<float> (image.getWidth()) * factor)),
                                    juce::jmax (1, static_cast<int> (static_cast<float> (image.getHeight()) * factor)));
        }

        texture.loadImage (image);
        artworkAspect = static_cast<float> (snapshot.image.getWidth())
                      / static_cast<float> (juce::jmax (1, snapshot.image.getHeight()));
        glowTint = snapshot.palette.accent;
    }

    void storeEffects (const EffectParams& e)
    {
        const std::array<float, effectFieldCount> values { e.scale,          e.brightness,      e.glow,  e.texture,
                                                           e.leftBrightness, e.rightBrightness, e.shift, e.depth };

        for (std::size_t i = 0; i < effectFieldCount; ++i)
            effectFields[i].store (values[i], std::memory_order_relaxed);
    }

    EffectParams loadEffects() const
    {
        EffectParams e;
        e.scale = juce::jmax (1.0f, effectFields[0].load (std::memory_order_relaxed));
        e.brightness = effectFields[1].load (std::memory_order_relaxed);
        e.glow = effectFields[2].load (std::memory_order_relaxed);
        e.texture = effectFields[3].load (std::memory_order_relaxed);
        e.leftBrightness = effectFields[4].load (std::memory_order_relaxed);
        e.rightBrightness = effectFields[5].load (std::memory_order_relaxed);
        e.shift = effectFields[6].load (std::memory_order_relaxed);
        e.depth = effectFields[7].load (std::memory_order_relaxed);
        return e;
    }

    //==========================================================================
    const RenderOverrides overrides;

    // Message thread.
    juce::Component* host = nullptr;
    bool wasAttachRequested = false;
    bool repainting = true;
    double resumedAtMs = 0.0;
    double attachedAtMs = 0.0;
    FailureCallback onFailure;

    // Shared.
    std::atomic<bool> ready { false };
    std::atomic<bool> failurePosted { false };
    std::atomic<std::int64_t> frames { 0 };
    std::atomic<double> heartbeatMs { -1.0 };
    std::atomic<double> averageFrameMs { 0.0 };
    std::atomic<int> swapInterval { 1 };
    std::atomic<double> firstFrameMs { -1.0 };
    std::atomic<double> lastFrameMs { -1.0 };
    std::atomic<double> maxFrameGapMs { 0.0 };
    std::atomic<int> glErrors { 0 };
    std::array<std::atomic<float>, effectFieldCount> effectFields;

    mutable std::mutex infoMutex;
    GpuInfo gpu;
    FallbackCause lastFailure = FallbackCause::none;
    juce::String lastFailureDetail;

    std::mutex artworkMutex;
    ArtworkInput artwork;
    std::uint64_t artworkGeneration = 0;

    // Render thread (released in openGLContextClosing).
    juce::OpenGLContext context;
    std::unique_ptr<juce::OpenGLShaderProgram> shader;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> uArtwork, uFit, uScale, uBrightness, uDepth, uGlow, uGrain,
        uSide, uShift, uTint;
    juce::OpenGLTexture texture;
    juce::uint32 vao = 0, vbo = 0;
    std::uint64_t uploadedGeneration = 0;
    float artworkAspect = 1.0f;
    juce::Colour glowTint = juce::Colours::white;
    int maxTextureSize = 0;
    bool hangDone = false;
};

//==============================================================================
GlRenderer::GlRenderer (RenderOverrides overrides)
    : impl (std::make_shared<Impl> (std::move (overrides)))
{
}

GlRenderer::~GlRenderer()
{
    impl->detach();
}

VisualRenderer::AttachResult GlRenderer::attach (juce::Component& host)
{
    return impl->attach (host);
}

void GlRenderer::detach()
{
    impl->detach();
}

void GlRenderer::setArtwork (const ArtworkInput& artwork)
{
    impl->setArtwork (artwork);
}

void GlRenderer::setEffects (const EffectParams& effects)
{
    impl->setEffects (effects);
}

void GlRenderer::tick()
{
    impl->tick();
}

RendererStatus GlRenderer::status() const
{
    return impl->status();
}

void GlRenderer::setFailureCallback (FailureCallback callback)
{
    impl->setFailureCallback (std::move (callback));
}
} // namespace snds::rendering
