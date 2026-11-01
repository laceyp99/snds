#include <juce_gui_basics/juce_gui_basics.h>

#include "ArtworkView.h"
#include "Options.h"

#if SNDS_PREFER_DISCRETE_GPU
// Ask NVIDIA Optimus / AMD switchable graphics to run this exe on the discrete GPU.
extern "C"
{
__declspec (dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec (dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

namespace snds::spike
{
namespace
{
constexpr int timerHz = 60;
constexpr double stallThresholdMs = 100.0;

struct GapStats
{
    double all = 0.0, startup = 0.0, attach = 0.0, transition = 0.0, steady = 0.0;
    int over50 = 0;
};

juce::var toVar (const juce::StringPairArray& pairs)
{
    auto* obj = new juce::DynamicObject();

    for (const auto& key : pairs.getAllKeys())
        obj->setProperty (key, pairs[key]);

    return juce::var (obj);
}

juce::DynamicObject* object() { return new juce::DynamicObject(); }

bool writePng (const juce::Image& image, const juce::File& file)
{
    if (! image.isValid())
        return false;

    file.deleteFile();
    juce::FileOutputStream out (file);
    return out.openedOk() && juce::PNGImageFormat().writeImageToStream (image, out);
}

//==============================================================================
class MainComponent final : public juce::Component,
                            private juce::Timer
{
public:
    explicit MainComponent (const Options& o)
        : options (o),
          artwork (o)
    {
        addAndMakeVisible (artwork);
        addAndMakeVisible (status);
        status.setColour (juce::Label::backgroundColourId, juce::Colour (0xff202020));
        status.setColour (juce::Label::textColourId, juce::Colours::white);
        setSize (960, 600);
        tickTimes.reserve (8192);
        startTimerHz (timerHz);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        status.setBounds (area.removeFromBottom (28));
        artwork.setBounds (area);
    }

private:
    void timerCallback() override
    {
        const auto now = nowMs();
        tickTimes.push_back (now);

        if (! visualsStarted && now >= 500.0)
        {
            visualsStarted = true;
            artwork.startVisuals();
        }

        artwork.onTimerTick();

        if (tickTimes.size() % 15 == 1)
            updateStatus();

        if (options.selfTestSeconds > 0.0 && now >= options.selfTestSeconds * 1000.0)
            finishSelfTest();
    }

    void updateStatus()
    {
        const auto& info = artwork.getGlInfo();
        status.setText ("tier: " + tierName (artwork.getTier())
                            + " | GL frames: " + juce::String (artwork.getGlFrames())
                            + " | CPU frames: " + juce::String (artwork.getCpuFrames())
                            + " | timer ticks: " + juce::String ((int) tickTimes.size())
                            + (info.renderer.isNotEmpty() ? " | " + info.renderer + " / " + info.version : juce::String()),
                        juce::dontSendNotification);
    }

    GapStats computeGaps() const
    {
        GapStats s;
        const auto attachStart = artwork.getAttachStartMs();
        const auto attachEnd = attachStart + artwork.getAttachCallMs();

        for (size_t i = 1; i < tickTimes.size(); ++i)
        {
            const auto begin = tickTimes[i - 1], end = tickTimes[i], gap = end - begin;
            s.all = juce::jmax (s.all, gap);

            if (gap > 50.0)
                ++s.over50;

            bool inTransition = false;

            for (const auto& t : artwork.getTransitions())
                inTransition = inTransition || (end >= t.atMs - 50.0 && begin <= t.completedAtMs + 500.0);

            const auto inAttach = attachStart >= 0.0 && end >= attachStart && begin <= attachEnd + 100.0;

            if (end < 400.0)        s.startup = juce::jmax (s.startup, gap);
            else if (inTransition)  s.transition = juce::jmax (s.transition, gap);
            else if (inAttach)      s.attach = juce::jmax (s.attach, gap);
            else                    s.steady = juce::jmax (s.steady, gap);
        }

        return s;
    }

    Tier expectedFinalTier() const
    {
        if (options.expectTier.isNotEmpty())
            return options.expectTier == "static" ? Tier::staticImage
                 : options.expectTier == "cpu"    ? Tier::cpu
                                                  : Tier::gl;

        if (options.forceCpuFailure)
            return Tier::staticImage;

        if (options.forceNoOpenGL || options.forceShaderFailure || options.forceContextLoss || options.forceGlHang)
            return Tier::cpu;

        return Tier::gl;
    }

    juce::String scenarioName() const
    {
        juce::StringArray parts;

        if (options.forceNoOpenGL)      parts.add ("force-no-opengl");
        if (options.forceShaderFailure) parts.add ("force-shader-failure");
        if (options.forceContextLoss)   parts.add ("force-context-loss");
        if (options.forceCpuFailure)    parts.add ("force-cpu-failure");
        if (options.forceGlHang)        parts.add ("force-gl-hang");
        if (options.requestGl != "3.2") parts.add ("request-gl-" + options.requestGl);
        if (options.requiredVersionString() != "3.2") parts.add ("require-gl-" + options.requiredVersionString());

        return parts.isEmpty() ? "normal" : parts.joinIntoString ("+");
    }

    void finishSelfTest()
    {
        if (finished)
            return;

        finished = true;
        stopTimer();

        const auto durationMs = nowMs();
        // The render thread is still running until the window closes: snapshot once.
        const auto glFrames = artwork.getGlFrames();
        const auto glFirstFrameMs = artwork.getGlFirstFrameMs();
        const auto glLastFrameMs = artwork.getGlLastFrameMs();
        const auto gaps = computeGaps();
        const auto& info = artwork.getGlInfo();
        const auto expected = expectedFinalTier();
        const auto finalTier = artwork.getTier();
        const auto firstTickMs = tickTimes.empty() ? durationMs : tickTimes.front();
        // Continuity is judged from the first tick; time before it is app/window startup.
        const auto expectedTicks = (durationMs - firstTickMs) * timerHz / 1000.0;
        const auto& captures = artwork.getCaptures();
        const auto animationVerified = captures.size() >= 2
                                    && std::abs (captures[0].meanLuma - captures[1].meanLuma) > 0.01;

        juce::Array<juce::var> checks;
        bool pass = true;

        const auto check = [&] (const juce::String& name, bool ok, const juce::String& detail)
        {
            auto* c = object();
            c->setProperty ("name", name);
            c->setProperty ("pass", ok);
            c->setProperty ("detail", detail);
            checks.add (juce::var (c));
            pass = pass && ok;
        };

        check ("final tier matches expectation", finalTier == expected,
               "final=" + tierName (finalTier) + " expected=" + tierName (expected));
        check ("timer kept ticking", (double) tickTimes.size() >= 0.8 * expectedTicks,
               juce::String ((int) tickTimes.size()) + " ticks, expected ~" + juce::String (expectedTicks, 0)
                   + " after first tick at " + juce::String (firstTickMs, 0) + " ms");
        check ("no steady-state timer stall", gaps.steady < stallThresholdMs,
               "max steady gap " + juce::String (gaps.steady, 1) + " ms");

        if (! artwork.getTransitions().empty())
            check ("no timer stall across fallback transitions", gaps.transition < stallThresholdMs,
                   "max transition gap " + juce::String (gaps.transition, 1) + " ms");

        if (expected == Tier::gl)
        {
            check ("context meets minimum", info.meetsMinimum,
                   info.version + " / GLSL " + info.glsl + " (" + info.profile + ")");
            check ("GL frames rendered", glFrames > 0, juce::String (glFrames));
            check ("shader animation visible in readback", animationVerified,
                   captures.size() >= 2 ? "luma " + juce::String (captures[0].meanLuma, 4) + " vs "
                                              + juce::String (captures[1].meanLuma, 4)
                                        : juce::String ("fewer than two captures"));
        }

        if (options.forceShaderFailure)
            check ("shader compile log captured", info.shaderLog.isNotEmpty(), info.shaderLog);

        if (options.forceContextLoss)
            check ("GL rendered before simulated loss", glFrames > 0, juce::String (glFrames));

        if (options.forceNoOpenGL)
            check ("OpenGL was not attempted", ! artwork.wasGlAttempted(), {});

        if (expected == Tier::staticImage)
            check ("CPU tier ran before static fallback", artwork.getCpuFrames() > 0, juce::String (artwork.getCpuFrames()));

        // ---- report ----
        const auto reportFile = options.reportFile;
        reportFile.getParentDirectory().createDirectory();
        const auto stem = reportFile.getFileNameWithoutExtension();
        const auto glPng = reportFile.getSiblingFile (stem + "-gl-frame.png");
        const auto finalPng = reportFile.getSiblingFile (stem + "-final.png");
        const auto wroteGlPng = writePng (artwork.getGlCaptureImage(), glPng);
        const auto wroteFinalPng = finalTier != Tier::gl
                                && writePng (artwork.createComponentSnapshot (artwork.getLocalBounds()), finalPng);

        juce::String renderingEngine;

        if (auto* peer = getPeer())
            renderingEngine = peer->getAvailableRenderingEngines()[peer->getCurrentRenderingEngine()];

        auto* root = object();
        root->setProperty ("spike", "opengl-tier");
        root->setProperty ("scenario", scenarioName());
        root->setProperty ("pass", pass);

        auto* build = object();
       #if JUCE_DEBUG
        build->setProperty ("config", "Debug");
       #else
        build->setProperty ("config", "Release");
       #endif
        build->setProperty ("juce", juce::SystemStats::getJUCEVersion());
        build->setProperty ("preferDiscreteGpuExport", (bool) SNDS_PREFER_DISCRETE_GPU);
        build->setProperty ("os", juce::SystemStats::getOperatingSystemName());
        build->setProperty ("cpu", juce::SystemStats::getCpuModel());
        root->setProperty ("build", juce::var (build));

        juce::StringPairArray opts;
        opts.set ("selfTestSeconds", juce::String (options.selfTestSeconds));
        opts.set ("requestGl", options.requestGl);
        opts.set ("requireGl", options.requiredVersionString());
        opts.set ("requireGlsl", juce::String (options.requiredGlslVersion(), 2));
        opts.set ("forceNoOpenGL", options.forceNoOpenGL ? "true" : "false");
        opts.set ("forceShaderFailure", options.forceShaderFailure ? "true" : "false");
        opts.set ("forceContextLoss", options.forceContextLoss ? juce::String (options.contextLossAfterMs, 0) : "false");
        opts.set ("forceCpuFailure", options.forceCpuFailure ? "true" : "false");
        opts.set ("forceGlHang", options.forceGlHang ? juce::String (options.glHangMs, 0) : "false");
        root->setProperty ("options", toVar (opts));
        root->setProperty ("durationMs", durationMs);
        root->setProperty ("renderingEngine", renderingEngine);

        auto* gl = object();
        gl->setProperty ("attempted", artwork.wasGlAttempted());
        gl->setProperty ("attachCallMs", artwork.getAttachCallMs());
        gl->setProperty ("contextCreated", info.contextCreated);
        gl->setProperty ("vendor", info.vendor);
        gl->setProperty ("renderer", info.renderer);
        gl->setProperty ("version", info.version);
        gl->setProperty ("glsl", info.glsl);
        gl->setProperty ("major", info.major);
        gl->setProperty ("minor", info.minor);
        gl->setProperty ("profile", info.profile);
        gl->setProperty ("contextFlags", info.contextFlags);
        gl->setProperty ("maxTextureSize", info.maxTextureSize);
        gl->setProperty ("meetsMinimum", info.meetsMinimum);
        gl->setProperty ("shaderCompiled", info.shaderCompiled);
        gl->setProperty ("shaderLog", info.shaderLog);
        gl->setProperty ("initMs", info.initMs);
        gl->setProperty ("frames", glFrames);
        gl->setProperty ("firstFrameAtMs", glFirstFrameMs);
        const auto glSpan = glLastFrameMs - glFirstFrameMs;
        gl->setProperty ("framesPerSecond", glSpan > 0.0 ? (double) glFrames * 1000.0 / glSpan : 0.0);
        gl->setProperty ("maxFrameGapMs", artwork.getGlMaxFrameGapMs());
        gl->setProperty ("errors", artwork.getGlErrorCount());

        juce::Array<juce::var> captureList;

        for (const auto& c : captures)
        {
            auto* cv = object();
            cv->setProperty ("atSeconds", c.atSeconds);
            cv->setProperty ("pulse", c.pulse);
            cv->setProperty ("meanLuma", c.meanLuma);
            captureList.add (juce::var (cv));
        }

        gl->setProperty ("captures", captureList);
        gl->setProperty ("animationVerified", animationVerified);
        gl->setProperty ("framePng", wroteGlPng ? glPng.getFullPathName() : juce::String());
        root->setProperty ("gl", juce::var (gl));

        auto* fallback = object();
        fallback->setProperty ("finalTier", tierName (finalTier));
        fallback->setProperty ("expectedFinalTier", tierName (expected));
        juce::Array<juce::var> transitionList;

        for (const auto& t : artwork.getTransitions())
        {
            auto* tv = object();
            tv->setProperty ("from", tierName (t.from));
            tv->setProperty ("to", tierName (t.to));
            tv->setProperty ("reason", t.reason);
            tv->setProperty ("atMs", t.atMs);
            tv->setProperty ("detachMs", t.detachMs);
            tv->setProperty ("completedAtMs", t.completedAtMs);
            transitionList.add (juce::var (tv));
        }

        fallback->setProperty ("transitions", transitionList);
        fallback->setProperty ("cpuFrames", artwork.getCpuFrames());
        fallback->setProperty ("cpuPaintAvgMs", artwork.getCpuFrames() > 0 ? artwork.getCpuPaintTotalMs() / artwork.getCpuFrames() : 0.0);
        fallback->setProperty ("cpuPaintMaxMs", artwork.getCpuPaintMaxMs());
        fallback->setProperty ("finalPng", wroteFinalPng ? finalPng.getFullPathName() : juce::String());
        root->setProperty ("fallback", juce::var (fallback));

        auto* timer = object();
        timer->setProperty ("hz", timerHz);
        timer->setProperty ("windowShownMs", windowShownMs);
        timer->setProperty ("firstTickMs", firstTickMs);
        timer->setProperty ("ticks", (int) tickTimes.size());
        timer->setProperty ("expectedTicks", expectedTicks);
        timer->setProperty ("maxGapMs", gaps.all);
        timer->setProperty ("maxGapStartupMs", gaps.startup);
        timer->setProperty ("maxGapAttachMs", gaps.attach);
        timer->setProperty ("maxGapTransitionMs", gaps.transition);
        timer->setProperty ("maxGapSteadyMs", gaps.steady);
        timer->setProperty ("gapsOver50Ms", gaps.over50);
        timer->setProperty ("stallThresholdMs", stallThresholdMs);
        root->setProperty ("timer", juce::var (timer));

        root->setProperty ("checks", checks);

        const auto json = juce::JSON::toString (juce::var (root), false);
        const auto wrote = reportFile.replaceWithText (json);

        juce::JUCEApplication::getInstance()->setApplicationReturnValue (pass && wrote ? 0 : 1);
        juce::JUCEApplication::quit();
    }

public:
    double windowShownMs = -1.0;

private:
    const Options options;
    ArtworkView artwork;
    juce::Label status;
    std::vector<double> tickTimes;
    bool visualsStarted = false;
    bool finished = false;
};

//==============================================================================
class MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow (const juce::String& name, const Options& options)
        : DocumentWindow (name, juce::Colours::black, DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        auto* content = new MainComponent (options);
        setContentOwned (content, true);
        setResizable (true, true);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        content->windowShownMs = nowMs();
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class SpikeApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String&) override
    {
        nowMs(); // start the clock
        mainWindow = std::make_unique<MainWindow> (getApplicationName(), Options::parse (getCommandLineParameterArray()));
    }

    void shutdown() override { mainWindow.reset(); }
    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted (const juce::String&) override {}

private:
    std::unique_ptr<MainWindow> mainWindow;
};
} // namespace
} // namespace snds::spike

START_JUCE_APPLICATION (snds::spike::SpikeApplication)
