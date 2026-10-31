#include "ArtworkView.h"

#include <cmath>

namespace snds::spike
{
namespace
{
// GLSL 1.50 is the shading language of OpenGL 3.2 core: the proposed minimum tier.
constexpr const char* vertexShaderSource = R"(#version 150
in vec2 position;
out vec2 uv;

void main()
{
    uv = position * 0.5 + 0.5;
    gl_Position = vec4 (position, 0.0, 1.0);
}
)";

constexpr const char* fragmentShaderSource = R"(#version 150
in vec2 uv;
out vec4 fragColour;

uniform sampler2D artwork;
uniform float pulse;
uniform vec2 aspect;

void main()
{
    vec2 st = (uv - 0.5) * aspect * (1.0 - 0.04 * pulse) + 0.5;

    if (any (lessThan (st, vec2 (0.0))) || any (greaterThan (st, vec2 (1.0))))
    {
        fragColour = vec4 (0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 base = texture (artwork, st).rgb;
    vec3 glow = vec3 (0.0);

    for (int i = 0; i < 8; ++i)
    {
        float a = 6.2831853 * float (i) / 8.0;
        glow += texture (artwork, st + (0.004 + 0.012 * pulse) * vec2 (cos (a), sin (a))).rgb;
    }

    glow /= 8.0;
    float vignette = smoothstep (0.85, 0.35, length (st - 0.5));
    vec3 colour = mix (base, max (base, glow) * 1.35, 0.5 * pulse);
    fragColour = vec4 (colour * (0.75 + 0.25 * vignette), 1.0);
}
)";

// Deliberate syntax error (missing semicolon) for --force-shader-failure.
constexpr const char* brokenFragmentShaderSource = R"(#version 150
out vec4 fragColour;

void main()
{
    fragColour = vec4 (1.0, 0.0, 1.0, 1.0)
}
)";

juce::Image createArtwork()
{
    // Software image: pixels are readable from the render thread for texture upload.
    juce::Image image (juce::Image::ARGB, 512, 512, true, juce::SoftwareImageType());
    juce::Graphics g (image);
    const auto bounds = image.getBounds().toFloat();

    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1d2b64), bounds.getTopLeft(),
                                             juce::Colour (0xfff8cdda), bounds.getBottomRight(), false));
    g.fillAll();

    for (int i = 0; i < 6; ++i)
    {
        const auto r = 220.0f - 34.0f * (float) i;
        g.setColour (juce::Colour::fromHSV (0.08f * (float) i, 0.7f, 0.95f, 0.85f));
        g.drawEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (bounds.getCentre()), 6.0f);
    }

    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (96.0f, juce::Font::bold));
    g.drawText ("SNDS", bounds, juce::Justification::centred);
    return image;
}

juce::String glString (GLenum name)
{
    const auto* s = juce::gl::glGetString (name);
    return s != nullptr ? juce::String::fromUTF8 (reinterpret_cast<const char*> (s)) : juce::String();
}
} // namespace

juce::String tierName (Tier t)
{
    switch (t)
    {
        case Tier::gl:          return "gl";
        case Tier::cpu:         return "cpu";
        case Tier::staticImage: return "static";
        case Tier::none:        break;
    }

    return "none";
}

double nowMs() noexcept
{
    // Function-local static: initialised on first call (from
    // JUCEApplication::initialise), never during static initialisation, when
    // JUCE's own high-resolution counter may not be set up yet.
    static const double startMs = juce::Time::getMillisecondCounterHiRes();
    return juce::Time::getMillisecondCounterHiRes() - startMs;
}

float pulseAt (double seconds) noexcept
{
    const auto beats = seconds * 2.0; // 120 BPM
    return (float) std::exp (-5.0 * (beats - std::floor (beats)));
}

//==============================================================================
ArtworkView::ArtworkView (const Options& o)
    : options (o),
      artworkSoftware (createArtwork()),
      artworkNative (juce::NativeImageType().convert (artworkSoftware))
{
    setOpaque (true);
}

ArtworkView::~ArtworkView()
{
    glContext.detach();
}

void ArtworkView::startVisuals()
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (options.forceNoOpenGL)
    {
        fallBack (Tier::cpu, "OpenGL skipped (--force-no-opengl)");
        return;
    }

    glAttempted = true;
    tier = Tier::gl;

    glContext.setRenderer (this);
    // Without component painting JUCE's render thread never takes the
    // MessageManager lock, so GL frames cannot stall message-thread timers.
    glContext.setComponentPaintingEnabled (false);
    glContext.setContinuousRepainting (true);

    if (options.requestGl == "default")  glContext.setOpenGLVersionRequired (juce::OpenGLContext::defaultGLVersion);
    else if (options.requestGl == "4.1") glContext.setOpenGLVersionRequired (juce::OpenGLContext::openGL4_1);
    else if (options.requestGl == "4.3") glContext.setOpenGLVersionRequired (juce::OpenGLContext::openGL4_3);
    else                                 glContext.setOpenGLVersionRequired (juce::OpenGLContext::openGL3_2);

    // Creates the native child window and WGL context synchronously on this thread.
    attachStartMs = nowMs();
    glContext.attachTo (*this);
    attachCallMs = nowMs() - attachStartMs;

    if (! glContext.isAttached())
        fallBack (Tier::cpu, "OpenGL native context creation failed");
}

void ArtworkView::onTimerTick()
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (tier == Tier::gl)
    {
        if (! glContext.isAttached())
        {
            fallBack (Tier::cpu, "OpenGL context no longer attached");
            return;
        }

        // A context that never initialises or a render thread that stops
        // producing frames is treated as failure, unless the window is hidden.
        const auto* peer = getPeer();
        const auto visible = isShowing() && peer != nullptr && ! peer->isMinimised();
        const auto last = glLastFrameMs.load();
        const auto reference = last >= 0.0 ? last : attachStartMs + attachCallMs;

        if (visible && nowMs() - reference > glWatchdogMs)
            fallBack (Tier::cpu, "watchdog: no OpenGL frame for " + juce::String (glWatchdogMs, 0) + " ms");
    }
    else if (tier == Tier::cpu)
    {
        if (options.forceCpuFailure && cpuFrames >= 30)
        {
            fallBack (Tier::staticImage, "CPU tier over budget (forced by --force-cpu-failure)");
            return;
        }

        if (recentCpuPaintMs.size() >= 30)
        {
            double sum = 0.0;

            for (auto ms : recentCpuPaintMs)
                sum += ms;

            const auto average = sum / (double) recentCpuPaintMs.size();

            if (average > cpuBudgetMs)
            {
                fallBack (Tier::staticImage, "CPU tier over budget: average paint "
                                                 + juce::String (average, 2) + " ms");
                return;
            }
        }

        repaint();
    }
}

void ArtworkView::paint (juce::Graphics& g)
{
    // Not called while the GL context is attached: JUCE replaces this
    // component's painting with the context's cached image.
    if (tier == Tier::cpu)
    {
        const auto start = nowMs();
        paintArtwork (g, pulseAt (start / 1000.0), true);
        const auto elapsed = nowMs() - start;

        ++cpuFrames;
        cpuPaintTotalMs += elapsed;
        cpuPaintMaxMs = juce::jmax (cpuPaintMaxMs, elapsed);
        recentCpuPaintMs.push_back (elapsed);

        if (recentCpuPaintMs.size() > 30)
            recentCpuPaintMs.erase (recentCpuPaintMs.begin());

        return;
    }

    paintArtwork (g, 0.0f, false);
}

void ArtworkView::paintArtwork (juce::Graphics& g, float pulse, bool animated)
{
    g.fillAll (juce::Colours::black);

    const auto area = getLocalBounds().toFloat();
    const auto side = juce::jmin (area.getWidth(), area.getHeight()) * (animated ? 1.0f + 0.04f * pulse : 1.0f);
    const auto dest = juce::Rectangle<float> (side, side).withCentre (area.getCentre());

    g.drawImage (artworkNative, dest, juce::RectanglePlacement::stretchToFit);

    if (animated)
    {
        g.setColour (juce::Colours::white.withAlpha (0.22f * pulse));
        g.fillRect (dest);

        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.35f * pulse), dest.getCentre(),
                                                 juce::Colours::transparentWhite, dest.getTopLeft(), true));
        g.fillEllipse (dest);
    }
}

void ArtworkView::fallBack (Tier to, const juce::String& reason)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if ((int) to <= (int) tier)
        return; // only ever move down the ladder

    Transition t;
    t.from = tier;
    t.to = to;
    t.reason = reason;
    t.atMs = nowMs();

    if (tier == Tier::gl)
    {
        glReady = false;
        const auto detachStart = nowMs();
        // Blocks until the render thread finishes its current frame, then calls
        // openGLContextClosing() on this thread.
        glContext.detach();
        t.detachMs = nowMs() - detachStart;
    }

    tier = to;
    recentCpuPaintMs.clear();
    repaint();
    t.completedAtMs = nowMs();
    transitions.push_back (t);
}

void ArtworkView::postGlFailure (const juce::String& reason)
{
    glReady = false;

    if (glFailurePosted.exchange (true))
        return;

    juce::MessageManager::callAsync ([safeThis = selfRef, reason]
    {
        if (safeThis != nullptr)
            safeThis->fallBack (Tier::cpu, reason);
    });
}

//==============================================================================
void ArtworkView::newOpenGLContextCreated()
{
    using namespace juce::gl;

    const auto start = nowMs();
    GlInfo info;
    info.contextCreated = true;
    info.vendor = glString (GL_VENDOR);
    info.renderer = glString (GL_RENDERER);
    info.version = glString (GL_VERSION);
    info.glsl = glString (GL_SHADING_LANGUAGE_VERSION);
    info.glslVersion = juce::OpenGLShaderProgram::getLanguageVersion();

    while (glGetError() != GL_NO_ERROR) {}

    GLint major = 0, minor = 0;
    glGetIntegerv (GL_MAJOR_VERSION, &major);
    glGetIntegerv (GL_MINOR_VERSION, &minor);

    if (glGetError() != GL_NO_ERROR || major <= 0)
    {
        // Pre-3.0 contexts (for example "GDI Generic" 1.1) lack GL_MAJOR_VERSION.
        const auto numbers = info.version.upToFirstOccurrenceOf (" ", false, false);
        major = numbers.upToFirstOccurrenceOf (".", false, false).getIntValue();
        minor = numbers.fromFirstOccurrenceOf (".", false, false).getIntValue();
    }

    info.major = (int) major;
    info.minor = (int) minor;
    const auto glVersion = info.major * 10 + info.minor;

    if (glVersion >= 30)
    {
        GLint contextFlagBits = 0;
        glGetIntegerv (GL_CONTEXT_FLAGS, &contextFlagBits);
        info.contextFlags = (int) contextFlagBits;
    }

    if (glVersion >= 32)
    {
        GLint mask = 0;
        glGetIntegerv (GL_CONTEXT_PROFILE_MASK, &mask);
        info.profile = (mask & GL_CONTEXT_CORE_PROFILE_BIT) != 0            ? "core"
                     : (mask & GL_CONTEXT_COMPATIBILITY_PROFILE_BIT) != 0   ? "compatibility"
                                                                            : "unknown";
    }
    else
    {
        info.profile = "legacy";
    }

    GLint maxTextureSize = 0;
    glGetIntegerv (GL_MAX_TEXTURE_SIZE, &maxTextureSize);
    info.maxTextureSize = (int) maxTextureSize;

    const auto requiredVersion = options.requireMajor * 10 + options.requireMinor;
    info.meetsMinimum = glVersion >= requiredVersion && info.glslVersion + 1.0e-6 >= options.requiredGlslVersion();

    juce::String failure;

    if (! info.meetsMinimum)
    {
        failure = "capability: OpenGL " + juce::String (info.major) + "." + juce::String (info.minor) + " / GLSL "
                + juce::String (info.glslVersion, 2) + " is below the required OpenGL "
                + options.requiredVersionString() + " / GLSL " + juce::String (options.requiredGlslVersion(), 2);
    }
    else
    {
        shader = std::make_unique<juce::OpenGLShaderProgram> (glContext);
        const auto* fragmentSource = options.forceShaderFailure ? brokenFragmentShaderSource : fragmentShaderSource;

        if (shader->addVertexShader (vertexShaderSource) && shader->addFragmentShader (fragmentSource) && shader->link())
        {
            info.shaderCompiled = true;
            uArtwork = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*shader, "artwork");
            uPulse = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*shader, "pulse");
            uAspect = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*shader, "aspect");

            texture.loadImage (artworkSoftware);

            const GLfloat quad[] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
            glGenVertexArrays (1, &vao);
            glBindVertexArray (vao);
            glGenBuffers (1, &vbo);
            glBindBuffer (GL_ARRAY_BUFFER, vbo);
            glBufferData (GL_ARRAY_BUFFER, sizeof (quad), quad, GL_STATIC_DRAW);
            const auto position = glGetAttribLocation (shader->getProgramID(), "position");
            glEnableVertexAttribArray ((GLuint) position);
            glVertexAttribPointer ((GLuint) position, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
            glBindVertexArray (0);
            glBindBuffer (GL_ARRAY_BUFFER, 0);

            if (const auto error = glGetError(); error != GL_NO_ERROR)
                failure = "OpenGL error 0x" + juce::String::toHexString ((int) error) + " during resource setup";
        }
        else
        {
            info.shaderLog = shader->getLastError().trim();
            failure = "GLSL compile/link failed";
            shader.reset();
        }
    }

    info.initMs = nowMs() - start;

    juce::MessageManager::callAsync ([safeThis = selfRef, info]
    {
        if (safeThis != nullptr)
            safeThis->glInfo = info;
    });

    if (failure.isNotEmpty())
        postGlFailure (failure);
    else
        glReady = true;
}

void ArtworkView::renderOpenGL()
{
    using namespace juce::gl;

    glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
    glClear (GL_COLOR_BUFFER_BIT);

    if (! glReady.load())
        return;

    const auto now = nowMs();
    auto first = glFirstFrameMs.load();

    if (first < 0.0)
    {
        first = now;
        glFirstFrameMs = now;
    }

    if (options.forceContextLoss && now - first >= options.contextLossAfterMs)
    {
        postGlFailure ("simulated context loss (--force-context-loss) after "
                       + juce::String (options.contextLossAfterMs, 0) + " ms of rendering");
        return;
    }

    // Diagnostic: simulate a driver hang/TDR by blocking the render thread once.
    if (options.forceGlHang && ! glHangDone && now - first >= 1500.0)
    {
        glHangDone = true;
        juce::Thread::sleep ((int) options.glHangMs);
    }

    GLint viewport[4] = {};
    glGetIntegerv (GL_VIEWPORT, viewport);
    const auto w = (float) juce::jmax (1, (int) viewport[2]);
    const auto h = (float) juce::jmax (1, (int) viewport[3]);
    const auto seconds = now / 1000.0;
    const auto pulse = pulseAt (seconds);

    shader->use();
    glActiveTexture (GL_TEXTURE0);
    texture.bind();
    uArtwork->set ((GLint) 0);
    uPulse->set ((GLfloat) pulse);
    uAspect->set (w >= h ? w / h : 1.0f, h > w ? h / w : 1.0f);

    glBindVertexArray (vao);
    glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray (0);
    texture.unbind();

    // Release builds get a KHR_no_error context from JUCE, so this is only
    // informative in Debug builds.
    if (const auto error = glGetError(); error != GL_NO_ERROR)
    {
        ++glErrorCount;

        if (error == GL_CONTEXT_LOST || error == GL_OUT_OF_MEMORY)
        {
            postGlFailure ("OpenGL error 0x" + juce::String::toHexString ((int) error) + " while rendering");
            return;
        }
    }

    if (options.selfTestSeconds > 0.0)
        captureFrame (seconds, pulse);

    if (const auto previous = glLastFrameMs.load(); previous >= 0.0)
        glMaxFrameGapMs = juce::jmax (glMaxFrameGapMs.load(), now - previous);

    ++glFrames;
    glLastFrameMs = now;
}

void ArtworkView::captureFrame (double seconds, float pulse)
{
    using namespace juce::gl;

    // Two readbacks after a 0.5 s warm-up: the first frame near a beat
    // (pulse >= 0.8), then the first frame between beats (pulse <= 0.15).
    // Gating on pulse rather than time keeps this robust to frame hitches.
    if (nextCaptureIndex >= 2 || seconds * 1000.0 < glFirstFrameMs.load() + 500.0)
        return;

    if (nextCaptureIndex == 0 ? pulse < 0.8f : pulse > 0.15f)
        return;

    GLint viewport[4] = {};
    glGetIntegerv (GL_VIEWPORT, viewport);
    const int w = juce::jmax (1, (int) viewport[2]);
    const int h = juce::jmax (1, (int) viewport[3]);

    juce::HeapBlock<juce::uint8> pixels ((size_t) w * (size_t) h * 4);
    glPixelStorei (GL_PACK_ALIGNMENT, 1);
    glReadPixels (0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.get());

    double lumaSum = 0.0;

    for (size_t i = 0; i < (size_t) w * (size_t) h; ++i)
        lumaSum += 0.2126 * pixels[i * 4] + 0.7152 * pixels[i * 4 + 1] + 0.0722 * pixels[i * 4 + 2];

    FrameCapture capture;
    capture.atSeconds = seconds;
    capture.pulse = pulse;
    capture.meanLuma = lumaSum / (255.0 * (double) w * (double) h);

    juce::Image image;

    if (nextCaptureIndex == 0)
    {
        image = juce::Image (juce::Image::RGB, w, h, false, juce::SoftwareImageType());
        juce::Image::BitmapData data (image, juce::Image::BitmapData::writeOnly);

        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                const auto* p = pixels.get() + ((size_t) y * (size_t) w + (size_t) x) * 4;
                data.setPixelColour (x, h - 1 - y, juce::Colour (p[0], p[1], p[2]));
            }
        }
    }

    ++nextCaptureIndex;

    juce::MessageManager::callAsync ([safeThis = selfRef, capture, image]
    {
        if (safeThis == nullptr)
            return;

        safeThis->captures.push_back (capture);

        if (image.isValid())
            safeThis->glCaptureImage = image;
    });
}

void ArtworkView::openGLContextClosing()
{
    using namespace juce::gl;

    glReady = false;

    if (vbo != 0)
        glDeleteBuffers (1, &vbo);

    if (vao != 0)
        glDeleteVertexArrays (1, &vao);

    vbo = vao = 0;
    texture.release();
    uArtwork.reset();
    uPulse.reset();
    uAspect.reset();
    shader.reset();
}
} // namespace snds::spike
