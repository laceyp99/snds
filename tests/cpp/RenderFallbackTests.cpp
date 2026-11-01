#include <juce_gui_basics/juce_gui_basics.h>

#include "rendering/CpuRenderer.h"
#include "rendering/FramePacing.h"
#include "rendering/VisualHost.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <limits>
#include <memory>

namespace snds::rendering
{
namespace
{
struct Pump
{
    Pump() { juce::MessageManager::getInstance()->setCurrentThreadAsMessageThread(); }

    static void run (int ms)
    {
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + ms;

        do
        {
            MSG message;

            while (PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage (&message);
                DispatchMessageW (&message);
            }

            juce::Thread::sleep (2);
        } while (juce::Time::getMillisecondCounterHiRes() < deadline);
    }

    template <typename Predicate>
    static bool until (Predicate&& done, int timeoutMs)
    {
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;

        while (juce::Time::getMillisecondCounterHiRes() < deadline)
        {
            run (10);

            if (done())
                return true;
        }

        return done();
    }
};

// Black art with a white square in the middle, so zoom and drift show up as moved edges.
ArtworkInput makeSquareArt (int size = 100)
{
    ArtworkInput input;
    input.image = juce::Image (juce::Image::ARGB, size, size, true);
    juce::Graphics g (input.image);
    g.fillAll (juce::Colours::black);
    g.setColour (juce::Colours::white);
    g.fillRect (size / 4, size / 4, size / 2, size / 2);
    input.palette.accent = juce::Colour (0xffff2000);
    return input;
}

ArtworkInput makeGreyArt (int size = 100)
{
    ArtworkInput input;
    input.image = juce::Image (juce::Image::ARGB, size, size, true);
    juce::Graphics g (input.image);
    g.fillAll (juce::Colour (0xff606060));
    input.palette.accent = juce::Colour (0xffff2000);
    return input;
}

juce::Image drawFrame (const ArtworkInput& art, const EffectParams& effects, int size = 100)
{
    juce::Image canvas (juce::Image::ARGB, size, size, true);
    juce::Graphics g (canvas);
    CpuRenderer::drawFrame (g, canvas.getBounds().toFloat(), art, effects);
    return canvas;
}

float luma (juce::Colour c)
{
    return 0.2126f * c.getFloatRed() + 0.7152f * c.getFloatGreen() + 0.0722f * c.getFloatBlue();
}

// What a fake renderer saw and what it reports (shared, so it outlives the renderer).
struct Record
{
    int attaches = 0, detaches = 0;
    double heartbeatAgeMs = -1.0; // >= 0 means "has a render thread"
    bool attached = false;
    double tickHz = 2.0;
    VisualRenderer::FailureCallback callback;

    void fail (FallbackCause cause, const juce::String& detail)
    {
        if (callback)
            callback (cause, detail);
    }
};

class FakeRenderer final : public VisualRenderer
{
public:
    FakeRenderer (RenderTier t, std::shared_ptr<Record> r)
        : fakeTier (t),
          record (std::move (r))
    {
    }

    RenderTier tier() const noexcept override { return fakeTier; }
    AttachResult attach (juce::Component&) override
    {
        ++record->attaches;
        record->attached = true;
        return {};
    }
    void detach() override
    {
        ++record->detaches;
        record->attached = false;
    }
    void setArtwork (const ArtworkInput&) override {}
    void setEffects (const EffectParams&) override {}
    void paint (juce::Graphics&) override {}
    void tick() override {}
    RendererStatus status() const override
    {
        RendererStatus s;
        s.tier = fakeTier;
        s.attached = record->attached;
        s.msSinceHeartbeat = record->heartbeatAgeMs;
        return s;
    }
    void setFailureCallback (FailureCallback c) override { record->callback = std::move (c); }
    double preferredTickHz() const noexcept override { return record->tickHz; }

    RenderTier fakeTier;
    std::shared_ptr<Record> record;
};

struct Fakes
{
    Fakes()
    {
        for (auto& r : records)
            r = std::make_shared<Record>();
    }

    VisualHost::RendererFactory factory()
    {
        return [this] (RenderTier tier) -> std::unique_ptr<VisualRenderer>
        {
            auto& record = records[static_cast<std::size_t> (tier)];
            record = std::make_shared<Record>();
            record->tickHz = tier == RenderTier::gl
                               ? pacing::glHealthHz
                               : (tier == RenderTier::cpu ? pacing::cpuFullHz : pacing::staticHealthHz);
            return std::make_unique<FakeRenderer> (tier, record);
        };
    }

    Record& get (RenderTier tier) const { return *records[static_cast<std::size_t> (tier)]; }

    std::array<std::shared_ptr<Record>, 4> records;
};

class RenderFallbackTests final : public juce::UnitTest
{
public:
    RenderFallbackTests()
        : juce::UnitTest ("Render fallbacks", "snds")
    {
    }

    void runTest() override
    {
        const Pump pump;
        testPacing();
        testCpuDrawing();
        testCpuBudget();
        testTimers();
        testHungRenderer();
        testOptInGl();
    }

private:
    void testPacing()
    {
        beginTest ("Frame pacing: the swap interval follows the display, the frame floor stops a spinning loop");

        using namespace pacing;
        expectEquals (swapIntervalFor (60.0), 1);
        expectEquals (swapIntervalFor (59.94), 1);
        expectEquals (swapIntervalFor (75.0), 1);
        expectEquals (swapIntervalFor (90.0), 1);
        expectEquals (swapIntervalFor (120.0), 2); // 60 fps
        expectEquals (swapIntervalFor (144.0), 2); // 72 fps
        expectEquals (swapIntervalFor (165.0), 2);
        expectEquals (swapIntervalFor (240.0), 4);
        expectEquals (swapIntervalFor (30.0), 1); // never below every refresh
        expectEquals (swapIntervalFor (0.0), 1);
        expectEquals (swapIntervalFor (-5.0), 1);
        expectEquals (swapIntervalFor (std::numeric_limits<double>::quiet_NaN()), 1);

        expectWithinAbsoluteError (minFrameIntervalMs(), 13.333, 0.01);
        expect (minFrameIntervalMs() < 1000.0 / targetFps); // a vsynced 60 Hz loop never has to sleep
        expect (minFrameIntervalMs (30.0) > minFrameIntervalMs (60.0));
        expect (targetFps == 60.0 && cpuFullHz == 60.0 && cpuReducedHz == 30.0);
    }

    void testCpuDrawing()
    {
        beginTest ("CPU tier effects: zoom, push-in, glow, side brightness and drift change the picture as intended");

        const auto art = makeSquareArt();
        const auto plain = drawFrame (art, {});

        expect (plain.getPixelAt (50, 50) == juce::Colours::white);
        expect (plain.getPixelAt (10, 50) == juce::Colours::black);
        expect (plain.getPixelAt (24, 50) == juce::Colours::black); // just outside the white square (25..74)

        EffectParams zoom;
        zoom.scale = 1.1f;
        const auto zoomed = drawFrame (art, zoom);
        expect (zoomed.getPixelAt (24, 50) == juce::Colours::white); // the square grew
        expect (zoomed.getPixelAt (50, 50) == juce::Colours::white);
        expect (zoomed.getPixelAt (3, 3) == juce::Colours::black);

        EffectParams depth;
        depth.depth = 1.0f;
        const auto grey = makeGreyArt();
        const auto pushed = drawFrame (grey, depth);
        const auto flat = drawFrame (grey, {});
        expect (luma (pushed.getPixelAt (2, 2)) < luma (flat.getPixelAt (2, 2)) * 0.9f); // darker edges
        expectWithinAbsoluteError (luma (pushed.getPixelAt (50, 50)), luma (flat.getPixelAt (50, 50)), 0.02f);

        EffectParams glow;
        glow.glow = 1.0f;
        const auto glowing = drawFrame (grey, glow);
        expect (glowing.getPixelAt (50, 50).getFloatRed()
                > flat.getPixelAt (50, 50).getFloatRed() + 0.1f); // the red accent
        expect (glowing.getPixelAt (50, 50).getFloatBlue() <= flat.getPixelAt (50, 50).getFloatBlue() + 0.02f);

        EffectParams left;
        left.leftBrightness = 1.0f;
        const auto leftLit = drawFrame (grey, left);
        expect (luma (leftLit.getPixelAt (3, 50)) > luma (flat.getPixelAt (3, 50)) + 0.05f);
        expectWithinAbsoluteError (luma (leftLit.getPixelAt (96, 50)), luma (flat.getPixelAt (96, 50)), 0.01f);

        EffectParams right;
        right.rightBrightness = 1.0f;
        const auto rightLit = drawFrame (grey, right);
        expect (luma (rightLit.getPixelAt (96, 50)) > luma (flat.getPixelAt (96, 50)) + 0.05f);
        expectWithinAbsoluteError (luma (rightLit.getPixelAt (3, 50)), luma (flat.getPixelAt (3, 50)), 0.01f);

        EffectParams drift;
        drift.scale = 1.1f;
        drift.shift = 1.0f;
        const auto drifted = drawFrame (art, drift);
        expect (! (drifted.getPixelAt (24, 50) == zoomed.getPixelAt (24, 50))
                || ! (drifted.getPixelAt (76, 50) == zoomed.getPixelAt (76, 50)));

        // Everything stays inside the artwork's own frame: no art outside it, bars stay black.
        const auto wide = drawFrame (art, {}, 100);
        juce::Image stretched (juce::Image::ARGB, 200, 100, true);
        {
            juce::Graphics g (stretched);
            CpuRenderer::drawFrame (g, stretched.getBounds().toFloat(), art, zoom);
        }
        expect (stretched.getPixelAt (5, 50) == juce::Colours::black); // letterbox bar
        expect (stretched.getPixelAt (195, 50) == juce::Colours::black);
        expect (stretched.getPixelAt (100, 50) == juce::Colours::white);
        juce::ignoreUnused (wide);

        // No artwork: black, never a crash.
        expect (drawFrame ({}, glow).getPixelAt (50, 50) == juce::Colours::black);
    }

    void testCpuBudget()
    {
        beginTest ("CPU tier budget: fast paints are fine, slow ones drop to 30 Hz, hopeless ones report once");

        struct Case
        {
            double paintMs;
            bool expectReduced;
            bool expectFailure;
        };

        for (const auto& c : { Case { 1.0, false, false }, Case { 10.0, true, false }, Case { 20.0, true, true } })
        {
            double now = 0.0;
            const auto step = c.paintMs;
            CpuRenderer cpu ({},
                             [&now, step]
                             {
                                 const auto value = now;
                                 now += step;
                                 return value;
                             });

            juce::Component host;
            host.setSize (100, 100);
            std::vector<std::pair<FallbackCause, juce::String>> failures;
            cpu.setFailureCallback ([&failures] (FallbackCause cause, const juce::String& detail)
                                    { failures.emplace_back (cause, detail); });
            expect (cpu.attach (host).ok);
            cpu.setArtwork (makeSquareArt());
            expectEquals (cpu.preferredTickHz(), 60.0);

            juce::Image canvas (juce::Image::ARGB, 100, 100, true);

            for (int frame = 0; frame < 200; ++frame)
            {
                juce::Graphics g (canvas);
                cpu.paint (g);
                cpu.tick();
            }

            const auto label = "paint " + juce::String (c.paintMs) + " ms";
            expect (cpu.isReduced() == c.expectReduced, label);
            expectEquals (cpu.preferredTickHz(), c.expectReduced ? 30.0 : 60.0);
            expectEquals (static_cast<int> (failures.size()), c.expectFailure ? 1 : 0); // once, however long it goes on

            if (c.expectFailure)
            {
                expect (failures[0].first == FallbackCause::cpuOverBudget && failures[0].second.contains ("20."),
                        label);
                expect (cpu.status().lastFailure == FallbackCause::cpuOverBudget);
            }

            expect (cpu.status().frames == 200 && cpu.status().attached);
            cpu.detach();
            expect (! cpu.status().attached);
        }

        beginTest ("CPU tier: the forced failure switch fires after 30 frames, however fast they are");

        RenderOverrides forced;
        forced.forceCpuFailure = true;
        CpuRenderer cpu (forced);
        juce::Component host;
        host.setSize (50, 50);
        int failures = 0;
        cpu.setFailureCallback ([&failures] (FallbackCause cause, const juce::String&)
                                { failures += cause == FallbackCause::cpuOverBudget; });
        cpu.attach (host);
        juce::Image canvas (juce::Image::ARGB, 50, 50, true);

        for (int frame = 0; frame < 29; ++frame)
        {
            juce::Graphics g (canvas);
            cpu.paint (g);
            cpu.tick();
        }
        expectEquals (failures, 0);

        for (int frame = 0; frame < 5; ++frame)
        {
            juce::Graphics g (canvas);
            cpu.paint (g);
            cpu.tick();
        }
        expectEquals (failures, 1);

        // A renderer that is not attached paints nothing and costs nothing.
        CpuRenderer idle;
        juce::Graphics g (canvas);
        idle.paint (g);
        expectEquals (static_cast<int> (idle.status().frames), 0);
    }

    void testTimers()
    {
        beginTest ("The host timer runs at the rate the current tier wants, and stops when there is nothing to do");

        Fakes fakes;
        fakes.records.fill (nullptr);
        VisualHost host ({}, fakes.factory());
        expectEquals (host.tickHz(), 0.0);

        host.start();
        expect (host.tier() == RenderTier::gl);
        expectEquals (host.tickHz(), pacing::glHealthHz);

        fakes.get (RenderTier::gl).fail (FallbackCause::contextLost, "lost");
        expect (Pump::until ([&] { return host.tier() == RenderTier::cpu; }, 2000));
        expectEquals (host.tickHz(), pacing::cpuFullHz); // only the CPU tier is a 60 Hz frame timer

        // The CPU tier slows itself down: the host follows after its next tick.
        fakes.get (RenderTier::cpu).tickHz = pacing::cpuReducedHz;
        host.tick();
        expectEquals (host.tickHz(), pacing::cpuReducedHz);

        fakes.get (RenderTier::cpu).fail (FallbackCause::cpuOverBudget, "slow");
        expect (Pump::until ([&] { return host.tier() == RenderTier::staticImage; }, 2000));
        expectEquals (host.tickHz(), pacing::staticHealthHz); // a still picture needs no frame timer

        host.stop();
        expectEquals (host.tickHz(), 0.0);
    }

    void testHungRenderer()
    {
        beginTest ("A hung GL renderer is never detached on the message thread; the next tier shows at once");

        Fakes fakes;
        VisualHost host ({}, fakes.factory());
        host.setSize (100, 100);
        host.start();
        auto& gl = fakes.get (RenderTier::gl);
        expect (host.isGlSurfaceVisible());

        gl.heartbeatAgeMs = 50.0;   // alive
        gl.heartbeatAgeMs = 6000.0; // ... and then the driver hangs
        const auto before = juce::Time::getMillisecondCounterHiRes();
        gl.fail (FallbackCause::watchdogTimeout, "no frame for 2000 ms");
        expect (Pump::until ([&] { return host.tier() == RenderTier::cpu; }, 2000));
        const auto tookMs = juce::Time::getMillisecondCounterHiRes() - before;

        expect (tookMs < 500.0);       // the swap is not held up by the dead render thread
        expectEquals (gl.detaches, 0); // detach() would block: it is deferred
        expectEquals (host.retiredCount(), 1);
        expect (! host.isGlSurfaceVisible());         // hidden, so the CPU tier shows through
        expect (host.tickHz() >= pacing::glHealthHz); // still polling for the thread to come back

        host.tick();
        expectEquals (gl.detaches, 0); // still hung

        // The driver answers again: the next tick detaches it, quickly, and the polling can stop.
        gl.heartbeatAgeMs = 20.0;
        host.tick();
        expectEquals (gl.detaches, 1);
        expectEquals (host.retiredCount(), 0);
        expectEquals (host.tickHz(), pacing::cpuFullHz);

        beginTest ("A GL renderer that fails but is alive (lost context, bad shader) is detached straight away");

        Fakes alive;
        VisualHost other ({}, alive.factory());
        other.start();
        alive.get (RenderTier::gl).heartbeatAgeMs = 5.0;
        alive.get (RenderTier::gl).fail (FallbackCause::shaderCompileFailed, "bad");
        expect (Pump::until ([&] { return other.tier() == RenderTier::cpu; }, 2000));
        expectEquals (alive.get (RenderTier::gl).detaches, 1);
        expectEquals (other.retiredCount(), 0);
        expect (! other.isGlSurfaceVisible());

        beginTest ("Stopping and destroying the host never waits for a hung renderer");

        Fakes stuck;
        VisualHost::setLeakHungRenderersOnShutdown (false); // tests free the fake; real hosts leak a hung driver object
        {
            VisualHost doomed ({}, stuck.factory());
            doomed.start();
            stuck.get (RenderTier::gl).heartbeatAgeMs = 9000.0;
            doomed.stop(); // would block on a real hung context
            expectEquals (doomed.retiredCount(), 1);
            expectEquals (stuck.get (RenderTier::gl).detaches, 0);
        } // destroyed with the hung renderer still waiting
        expectEquals (stuck.get (RenderTier::gl).detaches, 0);
        VisualHost::setLeakHungRenderersOnShutdown (true);
    }

    //==========================================================================
    // Real OpenGL (opt-in, like RenderHostTests): pacing and a driver that hangs mid-run.
    struct GapWatcher final : private juce::Timer
    {
        GapWatcher() { startTimer (10); }
        ~GapWatcher() override { stopTimer(); }

        void timerCallback() override
        {
            const auto now = juce::Time::getMillisecondCounterHiRes();

            if (last > 0.0 && armed)
                worstGapMs = std::max (worstGapMs, now - last);

            last = now;
        }

        double last = 0.0, worstGapMs = 0.0;
        bool armed = false;
    };

    void testOptInGl()
    {
        beginTest (
            "Real OpenGL: about 60 fps, no spinning, and a hung driver never freezes the message thread (opt-in)");

        if (juce::SystemStats::getEnvironmentVariable ("SNDS_RENDER_GL_TESTS", {}) != "1")
        {
            logMessage ("skipped: set SNDS_RENDER_GL_TESTS=1 to run the real OpenGL scenarios");
            return;
        }

        auto makeHost = [] (const char* switches)
        {
            auto host = std::make_unique<VisualHost> (RenderOverrides::parse (switches));
            host->setBounds (60, 60, 256, 256);
            ArtworkInput art = makeSquareArt (128);
            host->setArtwork (art);
            host->addToDesktop (juce::ComponentPeer::windowHasTitleBar);
            host->setVisible (true);
            Pump::run (100);
            host->start();
            return host;
        };

        {
            auto host = makeHost ("");
            expect (Pump::until ([&] { return host->status().frames > 5; }, 8000));
            const auto first = host->status().frames;
            Pump::run (2000);
            const auto second = host->status();
            const auto fps = static_cast<double> (second.frames - first) / 2.0;
            logMessage ("gl frame rate over 2 s: " + juce::String (fps, 1) + " fps, average frame "
                        + juce::String (second.averageFrameMs, 1) + " ms, max gap "
                        + juce::String (second.maxFrameGapMs, 1) + " ms, heartbeat age "
                        + juce::String (second.msSinceHeartbeat, 1) + " ms");
            expect (host->tier() == RenderTier::gl);
            expect (fps >= 25.0 && fps <= 80.0, "fps " + juce::String (fps));
            expect (second.averageFrameMs >= 12.0); // paced, not free-running
            expect (second.msSinceHeartbeat >= 0.0 && second.msSinceHeartbeat < 200.0);
            host->stop();
        }

        {
            // The render thread blocks for 3.5 s after 1.5 s of rendering.
            auto host = makeHost ("gl-hang=3500");
            expect (Pump::until ([&] { return host->status().frames > 5; }, 8000));
            GapWatcher watcher;
            watcher.armed = true;

            const auto start = juce::Time::getMillisecondCounterHiRes();
            expect (Pump::until ([&] { return host->tier() == RenderTier::cpu; }, 12000));
            const auto fellBackAfter = juce::Time::getMillisecondCounterHiRes() - start;
            logMessage ("fell back after " + juce::String (fellBackAfter, 0) + " ms; retired "
                        + juce::String (host->retiredCount()) + "; worst message-thread gap "
                        + juce::String (watcher.worstGapMs, 0) + " ms");

            expect (! host->events().empty() && host->events()[0].cause == FallbackCause::watchdogTimeout);
            expect (watcher.worstGapMs < 400.0,
                    "gap " + juce::String (watcher.worstGapMs)); // the UI kept ticking through the hang

            // Once the driver comes back (the hang ends), the old renderer is detached for good.
            expect (Pump::until ([&] { return host->retiredCount() == 0; }, 15000));
            logMessage ("retired renderer released; worst gap overall " + juce::String (watcher.worstGapMs, 0) + " ms");
            expect (watcher.worstGapMs < 400.0);
            expect (host->tier() == RenderTier::cpu);
            host->stop();
        }
    }
};

RenderFallbackTests renderFallbackTests;
} // namespace
} // namespace snds::rendering
