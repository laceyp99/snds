// spike-opengl-tier: OpenGL capability probe with automatic CPU fallback.
//
// Opens a small window, requests an OpenGL 3.2 core context through juce::OpenGLContext, renders a
// minimal animated GLSL 1.50 effect, and falls back to a JUCE-painted component when the context is
// not created in time, the version/GLSL tier is too low, the renderer is software-only (e.g. GDI
// Generic), or the shader fails to compile/link. A 60 Hz juce::Timer runs on the message thread the
// whole time; its tick gaps show whether probing or fallback disturbed the UI thread. The window
// closes by itself and the results are printed to stdout (and optionally written as JSON).
//
// Usage: snds_gl_probe [--run-ms=4000] [--context-timeout-ms=2500]
//                      [--force=none|context|version|shader] [--json=<file>]
//                      [--paint-engine=default|software|direct2d]
//
// Exit codes: 0 probe completed and the UI timer stayed healthy; 1 bad arguments;
//             3 UI timer was disturbed (a gap of 50 ms or more after the tier was decided);
//             4 the selected tier never presented a frame.

#include "rendering/OpenGLCapability.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>

#include <atomic>
#include <cmath>
#include <iostream>

namespace
{

using namespace juce::gl;
using snds::rendering::CapabilityRequirements;
using snds::rendering::CapabilityVerdict;
using snds::rendering::OpenGLInfo;

constexpr int timerIntervalMs = 16;
constexpr double disturbedGapMs = 50.0; // three missed 60 Hz frames
constexpr double settleMs = 250.0;      // grace period after the tier is decided before steady-state gaps count

enum class ForcedFailure
{
    none,
    context, ///< Keep the GL component hidden so no context is ever created (watchdog path).
    version, ///< Demand an impossible GL version (capability path).
    shader,  ///< Inject a GLSL syntax error (shader path).
};

const char* toString (ForcedFailure force)
{
    switch (force)
    {
        case ForcedFailure::none:
            return "none";
        case ForcedFailure::context:
            return "context";
        case ForcedFailure::version:
            return "version";
        case ForcedFailure::shader:
            return "shader";
    }

    return "unknown";
}

struct ProbeOptions
{
    int runMs = 4000;
    int contextTimeoutMs = 2500;
    ForcedFailure force = ForcedFailure::none;
    juce::String paintEngine = "default"; ///< JUCE window renderer used by the fallback tier.
    juce::File jsonOutput;
    juce::String error;

    static ProbeOptions parse (const juce::StringArray& args)
    {
        ProbeOptions options;

        for (const auto& arg : args)
        {
            const auto name = arg.upToFirstOccurrenceOf ("=", false, false);
            const auto value = arg.fromFirstOccurrenceOf ("=", false, false).unquoted();

            if (name == "--run-ms")
                options.runMs = juce::jlimit (500, 60000, value.getIntValue());
            else if (name == "--context-timeout-ms")
                options.contextTimeoutMs = juce::jlimit (100, 30000, value.getIntValue());
            else if (name == "--paint-engine" && (value == "default" || value == "software" || value == "direct2d"))
                options.paintEngine = value;
            else if (name == "--json")
                options.jsonOutput = juce::File::getCurrentWorkingDirectory().getChildFile (value);
            else if (name == "--force" && value == "none")
                options.force = ForcedFailure::none;
            else if (name == "--force" && value == "context")
                options.force = ForcedFailure::context;
            else if (name == "--force" && value == "version")
                options.force = ForcedFailure::version;
            else if (name == "--force" && value == "shader")
                options.force = ForcedFailure::shader;
            else
                options.error = "Unknown argument: " + arg;
        }

        return options;
    }
};

/** Everything learned on the GL thread, handed to the message thread by value. */
struct GlContextReport
{
    OpenGLInfo info;
    juce::String contextVersion;
    CapabilityVerdict verdict;
    bool shaderCompiled = false;
    juce::String shaderLog;
};

juce::String glString (GLenum name)
{
    const auto* text = glGetString (name);
    return text != nullptr ? juce::String::fromUTF8 (reinterpret_cast<const char*> (text)) : juce::String();
}

//==============================================================================
/** Minimal GLSL effect: a pulsing radial glow drawn with one full-screen triangle. */
class GlEffectView final : public juce::Component, private juce::OpenGLRenderer
{
public:
    /** Message thread: ok = first frame presented; otherwise reason describes the failure. */
    std::function<void (const GlContextReport&, bool ok, const juce::String& reason)> onResult;

    explicit GlEffectView (ForcedFailure forceIn)
        : force (forceIn)
    {
        setOpaque (true);
    }

    ~GlEffectView() override { detach(); }

    void attach()
    {
        context.setRenderer (this);
        context.setPreferredVersion ({ 3, 2 });
        context.setPreferredProfile (juce::OpenGLProfile::core);
        context.setComponentPaintingEnabled (false);
        context.setContinuousRepainting (true);
        context.attachTo (*this);
    }

    /** Message thread only. Blocks until the GL thread has stopped. */
    void detach() { context.detach(); }

    int getFramesRendered() const noexcept { return frames.load(); }

    void resized() override
    {
        width = getWidth();
        height = getHeight();
    }

private:
    void newOpenGLContextCreated() override
    {
        GlContextReport report;
        report.info.vendor = glString (GL_VENDOR);
        report.info.renderer = glString (GL_RENDERER);
        report.info.version = glString (GL_VERSION);
        report.info.shadingLanguageVersion = glString (GL_SHADING_LANGUAGE_VERSION);
        report.info.coreProfile = context.getProfile() == juce::OpenGLProfile::core;
        report.contextVersion =
            juce::String (context.getVersion().major) + "." + juce::String (context.getVersion().minor);

        while (glGetError() != GL_NO_ERROR) // e.g. GL_SHADING_LANGUAGE_VERSION on GL 1.1
        {
        }

        CapabilityRequirements requirements;

        if (force == ForcedFailure::version)
            requirements.minimumGl = { 9, 9 };

        report.verdict = snds::rendering::evaluateCapability (report.info, requirements);

        if (! report.verdict.meetsMinimum)
        {
            failed = true;
            post (report, false, "capability: " + report.verdict.reasons.joinIntoString (" "));
            return;
        }

        report.shaderCompiled = buildResources (report.shaderLog);

        if (! report.shaderCompiled)
        {
            failed = true;
            post (report, false, "shader: " + report.shaderLog.trim());
            return;
        }

        pendingReport = report;
    }

    void renderOpenGL() override
    {
        if (failed || program == nullptr)
            return;

        const auto scale = context.getRenderingScale();
        glViewport (0, 0, juce::roundToInt (scale * width.load()), juce::roundToInt (scale * height.load()));
        glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
        glClear (GL_COLOR_BUFFER_BIT);

        program->use();
        program->setUniform ("time",
                             static_cast<GLfloat> ((juce::Time::getMillisecondCounterHiRes() - startMs) / 1000.0));
        glBindVertexArray (vertexArray);
        glDrawArrays (GL_TRIANGLES, 0, 3);
        glBindVertexArray (0);

        if (frames.fetch_add (1) == 0)
        {
            const auto error = glGetError();

            if (error != GL_NO_ERROR)
            {
                failed = true;
                post (pendingReport,
                      false,
                      "GL error 0x" + juce::String::toHexString (static_cast<int> (error)) + " after the first frame");
                return;
            }

            post (pendingReport, true, {});
        }
    }

    void openGLContextClosing() override
    {
        program.reset();

        if (vertexBuffer != 0)
            glDeleteBuffers (1, &vertexBuffer);

        if (vertexArray != 0)
            glDeleteVertexArrays (1, &vertexArray);

        vertexBuffer = 0;
        vertexArray = 0;
    }

    bool buildResources (juce::String& log)
    {
        static constexpr const char* vertexShader = R"(#version 150
in vec2 position;
out vec2 uv;
void main()
{
    uv = position * 0.5 + 0.5;
    gl_Position = vec4 (position, 0.0, 1.0);
})";

        juce::String fragmentShader = R"(#version 150
uniform float time;
in vec2 uv;
out vec4 fragColour;
void main()
{
    float pulse = 0.5 + 0.5 * sin (time * 3.0);
    float glow = smoothstep (0.75, 0.0, distance (uv, vec2 (0.5)));
    vec3 colour = mix (vec3 (0.10, 0.12, 0.30), vec3 (0.95, 0.45, 0.20), glow * (0.4 + 0.6 * pulse));
    fragColour = vec4 (colour, 1.0);
})";

        if (force == ForcedFailure::shader)
            fragmentShader += "\nthis is not glsl;\n";

        auto shader = std::make_unique<juce::OpenGLShaderProgram> (context);

        if (! shader->addVertexShader (vertexShader) || ! shader->addFragmentShader (fragmentShader)
            || ! shader->link())
        {
            log = shader->getLastError();
            return false;
        }

        static constexpr GLfloat fullScreenTriangle[] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };

        glGenVertexArrays (1, &vertexArray);
        glBindVertexArray (vertexArray);
        glGenBuffers (1, &vertexBuffer);
        glBindBuffer (GL_ARRAY_BUFFER, vertexBuffer);
        glBufferData (GL_ARRAY_BUFFER, sizeof (fullScreenTriangle), fullScreenTriangle, GL_STATIC_DRAW);

        const auto location = glGetAttribLocation (shader->getProgramID(), "position");

        if (location < 0)
        {
            log = "The vertex attribute 'position' was not found.";
            return false;
        }

        glEnableVertexAttribArray (static_cast<GLuint> (location));
        glVertexAttribPointer (static_cast<GLuint> (location), 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindVertexArray (0);

        program = std::move (shader);
        return true;
    }

    void post (const GlContextReport& report, bool ok, const juce::String& reason)
    {
        juce::MessageManager::callAsync (
            [safe = self, report, ok, reason]
            {
                if (auto* view = safe.getComponent(); view != nullptr && view->onResult)
                    view->onResult (report, ok, reason);
            });
    }

    juce::OpenGLContext context;
    const ForcedFailure force;
    const double startMs = juce::Time::getMillisecondCounterHiRes();
    const SafePointer<GlEffectView> self { this }; // created on the message thread, only copied elsewhere

    std::atomic<int> width { 0 };
    std::atomic<int> height { 0 };
    std::atomic<int> frames { 0 };

    // GL thread only.
    std::unique_ptr<juce::OpenGLShaderProgram> program;
    GLuint vertexArray = 0;
    GLuint vertexBuffer = 0;
    bool failed = false;
    GlContextReport pendingReport;
};

//==============================================================================
/** CPU-light fallback tier: a JUCE-painted pulse driven by the message-thread timer. */
class FallbackView final : public juce::Component
{
public:
    void setPhase (double seconds)
    {
        phase = seconds;
        repaint();
    }

    void setReason (const juce::String& newReason) { reason = newReason; }

    int getPaintCount() const noexcept { return paints; }
    double getAveragePaintMs() const noexcept { return paints > 0 ? totalPaintMs / paints : 0.0; }

    void paint (juce::Graphics& g) override
    {
        ++paints;
        const auto paintStart = juce::Time::getMillisecondCounterHiRes();
        const auto pulse = 0.5f + 0.5f * std::sin (static_cast<float> (phase) * 3.0f);
        const auto bounds = getLocalBounds().toFloat();

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xfff27333).withAlpha (0.4f + 0.6f * pulse),
                                                 bounds.getCentre(),
                                                 juce::Colour (0xff1a1f4d),
                                                 bounds.getTopLeft(),
                                                 true));
        g.fillAll();
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (15.0f));
        g.drawFittedText ("CPU fallback tier\n" + reason,
                          getLocalBounds().reduced (16),
                          juce::Justification::bottomLeft,
                          5);
        totalPaintMs += juce::Time::getMillisecondCounterHiRes() - paintStart;
    }

private:
    double phase = 0.0;
    juce::String reason;
    int paints = 0;
    double totalPaintMs = 0.0;
};

//==============================================================================
class ProbeComponent final : public juce::Component, private juce::Timer
{
public:
    explicit ProbeComponent (ProbeOptions optionsIn)
        : options (std::move (optionsIn)),
          glView (options.force)
    {
        if (options.force == ForcedFailure::context)
            addChildComponent (glView); // hidden: JUCE never creates a context for it
        else
            addAndMakeVisible (glView);

        addChildComponent (fallbackView);
        glView.onResult = [this] (const GlContextReport& report, bool ok, const juce::String& reason)
        { handleGlResult (report, ok, reason); };

        setSize (640, 360);
        startMs = lastTickMs = juce::Time::getMillisecondCounterHiRes();
        startTimer (timerIntervalMs);
        glView.attach();
    }

    ~ProbeComponent() override
    {
        stopTimer();
        glView.detach();
    }

    void resized() override
    {
        glView.setBounds (getLocalBounds());
        fallbackView.setBounds (getLocalBounds());
    }

private:
    enum class Tier
    {
        pending,
        gpu,
        fallback,
    };

    static const char* tierName (Tier tier)
    {
        switch (tier)
        {
            case Tier::pending:
                return "pending";
            case Tier::gpu:
                return "gpu-shaders";
            case Tier::fallback:
                return "cpu-fallback";
        }

        return "unknown";
    }

    static double now() { return juce::Time::getMillisecondCounterHiRes(); }

    juce::String getPaintEngineName()
    {
        if (auto* peer = getPeer())
            return peer->getAvailableRenderingEngines()[peer->getCurrentRenderingEngine()];

        return "none";
    }

    void timerCallback() override
    {
        const auto tickMs = now();
        const auto gap = tickMs - lastTickMs;
        lastTickMs = tickMs;
        ++ticks;
        const auto elapsed = tickMs - startMs;

        if (gap > maxGapMs)
        {
            maxGapMs = gap;
            maxGapEndedAtMs = elapsed;
        }

        if (switchedAtMs > 0.0 && tickMs - switchedAtMs <= 1000.0)
            maxGapAfterSwitchMs = juce::jmax (maxGapAfterSwitchMs, gap);

        if (settledAtMs > 0.0 && tickMs - settledAtMs > settleMs)
            maxSteadyGapMs = juce::jmax (maxSteadyGapMs, gap);

        if (tier == Tier::pending && elapsed > options.contextTimeoutMs)
            switchToFallback ("context: no OpenGL context was ready within " + juce::String (options.contextTimeoutMs)
                              + " ms");

        if (tier == Tier::fallback)
            fallbackView.setPhase (elapsed / 1000.0);

        if (elapsed >= options.runMs)
            finish();
    }

    void handleGlResult (const GlContextReport& report, bool ok, const juce::String& reason)
    {
        glReport = report;
        haveGlReport = true;

        if (tier != Tier::pending)
            return;

        if (ok)
        {
            tier = Tier::gpu;
            gpuReadyAfterMs = now() - startMs;
            settledAtMs = now();
        }
        else
        {
            switchToFallback (reason);
        }
    }

    void switchToFallback (const juce::String& reason)
    {
        if (tier == Tier::fallback)
            return;

        tier = Tier::fallback;
        fallbackReason = reason;

        const auto before = now();
        glView.detach(); // stops the GL thread; must happen on the message thread
        detachMs = now() - before;

        glView.setVisible (false);
        fallbackView.setReason (reason);
        fallbackView.setVisible (true);
        switchedAtMs = now();
        settledAtMs = switchedAtMs;
    }

    void finish()
    {
        stopTimer();
        const auto framesRendered = glView.getFramesRendered();
        glView.detach();

        const auto elapsed = now() - startMs;
        const auto expectedTicks = static_cast<int> (elapsed / timerIntervalMs);
        const auto presented = tier == Tier::gpu ? framesRendered > 0 : fallbackView.getPaintCount() > 0;
        // The fallback switch and steady state must not stall the timer. The initial gap while JUCE
        // creates the native GL window/context on the message thread is reported separately.
        const auto timerHealthy = maxGapAfterSwitchMs < disturbedGapMs && maxSteadyGapMs < disturbedGapMs;
        const auto exitCode = ! presented ? 4 : (! timerHealthy ? 3 : 0);

        auto* json = new juce::DynamicObject();
        json->setProperty ("forcedFailure", toString (options.force));
        json->setProperty ("requested", "OpenGL 3.2 core");
        json->setProperty ("contextCreated", haveGlReport);
        json->setProperty ("glVendor", glReport.info.vendor);
        json->setProperty ("glRenderer", glReport.info.renderer);
        json->setProperty ("glVersion", glReport.info.version);
        json->setProperty ("glslVersion", glReport.info.shadingLanguageVersion);
        json->setProperty ("contextVersion", glReport.contextVersion);
        json->setProperty ("coreProfile", glReport.info.coreProfile);
        json->setProperty ("softwareRenderer", glReport.verdict.softwareRenderer);
        json->setProperty ("meetsMinimum", glReport.verdict.meetsMinimum);
        json->setProperty ("verdictReasons", glReport.verdict.reasons.joinIntoString (" "));
        json->setProperty ("shaderCompiled", glReport.shaderCompiled);
        json->setProperty ("shaderLog", glReport.shaderLog.trim());
        json->setProperty ("tier", tierName (tier));
        json->setProperty ("paintEngine", getPaintEngineName());
        json->setProperty ("fallbackReason", fallbackReason);
        json->setProperty ("gpuReadyAfterMs", gpuReadyAfterMs);
        json->setProperty ("gpuFramesRendered", framesRendered);
        json->setProperty ("fallbackPaints", fallbackView.getPaintCount());
        json->setProperty ("fallbackAveragePaintMs", fallbackView.getAveragePaintMs());
        json->setProperty ("detachMs", detachMs);
        json->setProperty ("timerTicks", ticks);
        json->setProperty ("timerExpectedTicks", expectedTicks);
        json->setProperty ("timerMaxGapMs", maxGapMs);
        json->setProperty ("timerMaxGapEndedAtMs", maxGapEndedAtMs);
        json->setProperty ("timerMaxSteadyGapMs", maxSteadyGapMs);
        json->setProperty ("timerMaxGapAfterFallbackMs", maxGapAfterSwitchMs);
        json->setProperty ("timerHealthy", timerHealthy);
        json->setProperty ("exitCode", exitCode);
        const juce::var result (json);

        std::cout << "SNDS GL probe (" << juce::SystemStats::getJUCEVersion() << ")\n"
                  << "  forced failure    : " << toString (options.force) << "\n"
                  << "  requested         : OpenGL 3.2 core, GLSL 1.50\n"
                  << "  context created   : " << (haveGlReport ? "yes" : "no") << "\n"
                  << "  GL_VENDOR         : " << glReport.info.vendor << "\n"
                  << "  GL_RENDERER       : " << glReport.info.renderer << "\n"
                  << "  GL_VERSION        : " << glReport.info.version << "\n"
                  << "  GLSL              : " << glReport.info.shadingLanguageVersion << "\n"
                  << "  context version   : " << glReport.contextVersion
                  << (glReport.info.coreProfile ? " core" : " compatibility") << "\n"
                  << "  meets minimum     : " << (glReport.verdict.meetsMinimum ? "yes" : "no") << " "
                  << glReport.verdict.reasons.joinIntoString (" ") << "\n"
                  << "  tier              : " << tierName (tier) << "\n"
                  << "  paint engine      : " << getPaintEngineName() << "\n"
                  << "  fallback reason   : " << fallbackReason << "\n"
                  << "  GPU frames        : " << framesRendered << "\n"
                  << "  fallback paints   : " << fallbackView.getPaintCount() << " (average "
                  << juce::String (fallbackView.getAveragePaintMs(), 1) << " ms in paint())\n"
                  << "  detach took       : " << juce::String (detachMs, 1) << " ms\n"
                  << "  timer ticks       : " << ticks << " of ~" << expectedTicks << " expected\n"
                  << "  timer max gap     : " << juce::String (maxGapMs, 1) << " ms, ending at "
                  << juce::roundToInt (maxGapEndedAtMs) << " ms\n"
                  << "  gap after fallback: " << juce::String (maxGapAfterSwitchMs, 1)
                  << " ms (within 1 s of the switch)\n"
                  << "  steady-state gap  : " << juce::String (maxSteadyGapMs, 1) << " ms (from " << settleMs
                  << " ms after the tier was decided)\n"
                  << "  result            : " << (exitCode == 0 ? "PASS" : "FAIL") << " (exit " << exitCode << ")"
                  << std::endl;

        if (options.jsonOutput != juce::File())
        {
            options.jsonOutput.getParentDirectory().createDirectory();
            options.jsonOutput.replaceWithText (juce::JSON::toString (result));
        }

        juce::JUCEApplicationBase::getInstance()->setApplicationReturnValue (exitCode);
        juce::JUCEApplicationBase::quit();
    }

    const ProbeOptions options;
    GlEffectView glView;
    FallbackView fallbackView;

    Tier tier = Tier::pending;
    GlContextReport glReport;
    bool haveGlReport = false;
    juce::String fallbackReason;

    double startMs = 0.0;
    double lastTickMs = 0.0;
    double switchedAtMs = 0.0;
    double gpuReadyAfterMs = -1.0;
    double detachMs = 0.0;
    double maxGapMs = 0.0;
    double maxGapAfterSwitchMs = 0.0;
    double maxGapEndedAtMs = 0.0;
    double maxSteadyGapMs = 0.0;
    double settledAtMs = 0.0;
    int ticks = 0;
};

//==============================================================================
class GlProbeApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "SNDS GL Probe"; }
    const juce::String getApplicationVersion() override { return SNDS_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String&) override
    {
        const auto options = ProbeOptions::parse (getCommandLineParameterArray());

        if (options.error.isNotEmpty())
        {
            std::cerr << options.error << std::endl;
            setApplicationReturnValue (1);
            quit();
            return;
        }

        window = std::make_unique<juce::DocumentWindow> ("SNDS GL probe", juce::Colours::black, 0);
        window->setUsingNativeTitleBar (true);
        window->setContentOwned (new ProbeComponent (options), true);
        window->centreWithSize (window->getWidth(), window->getHeight());
        window->setVisible (true);

        // Engine order on Windows: 0 = GDI software renderer, 1 = Direct2D (JUCE's default).
        if (auto* peer = window->getPeer(); peer != nullptr && options.paintEngine != "default")
            peer->setCurrentRenderingEngine (options.paintEngine == "software" ? 0 : 1);
    }

    void shutdown() override { window = nullptr; }
    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted (const juce::String&) override {}

private:
    std::unique_ptr<juce::DocumentWindow> window;
};

} // namespace

START_JUCE_APPLICATION (GlProbeApplication)
