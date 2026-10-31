#include <juce_gui_basics/juce_gui_basics.h>

#include "rendering/VisualHost.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <memory>

namespace snds::rendering
{
namespace
{
// JUCE delivers callAsync through the message queue, so the test pumps it.
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

// What a fake renderer saw. Shared, so a test can read it after the host has destroyed the renderer.
struct Record
{
    int attaches = 0, detaches = 0, artworkCalls = 0, effectCalls = 0, paints = 0, ticks = 0;
    ArtworkInput artwork;
    EffectParams effects;
    VisualRenderer::FailureCallback callback;

    void fail (FallbackCause cause, const juce::String& detail)
    {
        if (callback)
            callback (cause, detail);
    }
};

// A renderer with a scripted attach and a failure on demand.
class FakeRenderer final : public VisualRenderer
{
public:
    FakeRenderer (RenderTier t, std::shared_ptr<Record> r, bool attachOk, FallbackCause cause)
        : fakeTier (t),
          record (std::move (r)),
          ok (attachOk),
          failureCause (cause)
    {
    }

    RenderTier tier() const noexcept override { return fakeTier; }

    AttachResult attach (juce::Component&) override
    {
        ++record->attaches;
        return ok ? AttachResult() : AttachResult { false, failureCause, "scripted attach failure" };
    }

    void detach() override { ++record->detaches; }
    void setArtwork (const ArtworkInput& a) override
    {
        record->artwork = a;
        ++record->artworkCalls;
    }
    void setEffects (const EffectParams& e) override
    {
        record->effects = e;
        ++record->effectCalls;
    }
    void paint (juce::Graphics&) override { ++record->paints; }
    void tick() override { ++record->ticks; }
    RendererStatus status() const override
    {
        RendererStatus s;
        s.tier = fakeTier;
        s.attached = record->attaches > record->detaches;
        return s;
    }
    void setFailureCallback (FailureCallback c) override { record->callback = std::move (c); }

    RenderTier fakeTier;
    std::shared_ptr<Record> record;
    bool ok;
    FallbackCause failureCause;
};

// Hands out fakes and keeps a record for each tier.
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
            if (tier == RenderTier::cpu && ! cpuAvailable)
                return nullptr;

            const auto attachOk = tier == RenderTier::gl ? glAttachOk : (tier == RenderTier::cpu ? cpuAttachOk : true);
            const auto cause =
                tier == RenderTier::gl ? FallbackCause::belowMinimumVersion : FallbackCause::contextCreationFailed;
            auto& record = records[static_cast<std::size_t> (tier)];
            record = std::make_shared<Record>();
            return std::make_unique<FakeRenderer> (tier, record, attachOk, cause);
        };
    }

    Record& get (RenderTier tier) const { return *records[static_cast<std::size_t> (tier)]; }

    bool glAttachOk = true;
    bool cpuAvailable = false;
    bool cpuAttachOk = true;
    std::array<std::shared_ptr<Record>, 4> records;
};

ArtworkInput makeArtwork (juce::Colour colour, int size = 64)
{
    ArtworkInput input;
    input.image = juce::Image (juce::Image::ARGB, size, size, true);
    juce::Graphics g (input.image);
    g.fillAll (colour);
    input.palette = derivePalette (input.image);
    return input;
}

class RenderHostTests final : public juce::UnitTest
{
public:
    RenderHostTests()
        : juce::UnitTest ("Render host", "snds")
    {
    }

    void runTest() override
    {
        const Pump pump;
        testStartAndDelivery();
        testAttachFailures();
        testRuntimeFailure();
        testLadderNeverGoesUp();
        testLifetime();
        testRealStaticTier();
        testOptInGl();
    }

private:
    void testStartAndDelivery()
    {
        beginTest ("The best tier that attaches is used, and artwork and effects reach it");

        Fakes fakes;
        VisualHost host ({}, fakes.factory());
        host.setSize (100, 100);

        EffectParams effects;
        effects.glow = 0.5f;
        host.setArtwork (makeArtwork (juce::Colours::red));
        host.setEffects (effects); // before start: remembered
        host.start();

        expect (host.tier() == RenderTier::gl);
        expect (host.events().empty());
        auto& gl = fakes.get (RenderTier::gl);
        expect (gl.attaches == 1);
        expect (gl.artwork.isValid());
        expectEquals (gl.effects.glow, 0.5f);

        effects.glow = 0.9f;
        host.setEffects (effects);
        expectEquals (gl.effects.glow, 0.9f);
        expect (host.status().tier == RenderTier::gl);

        host.tick();
        expectEquals (gl.ticks, 1);

        host.start(); // a second start does nothing
        expectEquals (gl.attaches, 1);

        host.stop();
        expectEquals (gl.detaches, 1);
        expect (host.tier() == RenderTier::none);
    }

    void testAttachFailures()
    {
        beginTest ("A tier that cannot attach is skipped with a recorded reason; the static tier always works");

        Fakes fakes;
        fakes.glAttachOk = false;
        std::vector<FallbackEvent> heard;
        VisualHost host ({}, fakes.factory());
        host.setFallbackListener ([&heard] (const FallbackEvent& e) { heard.push_back (e); });
        host.setArtwork (makeArtwork (juce::Colours::blue));
        host.start();

        expect (host.tier() == RenderTier::staticImage);
        expectEquals (static_cast<int> (host.events().size()), 2);
        expect (host.events()[0].from == RenderTier::gl && host.events()[0].to == RenderTier::cpu);
        expect (host.events()[0].cause == FallbackCause::belowMinimumVersion);
        expect (host.events()[0].detail.contains ("scripted"));
        expect (host.events()[1].from == RenderTier::cpu && host.events()[1].to == RenderTier::staticImage);
        expect (host.events()[1].cause == FallbackCause::unavailable); // no cpu tier here
        expectEquals (static_cast<int> (heard.size()), 2);
        expectEquals (fakes.get (RenderTier::gl).detaches, 1); // the failed attach was cleaned up
        expect (fakes.get (RenderTier::staticImage).artwork.isValid());
    }

    void testRuntimeFailure()
    {
        beginTest ("A failure after attach moves down one tier, once, and the new tier gets the art and effects");

        Fakes fakes;
        fakes.cpuAvailable = true;
        VisualHost host ({}, fakes.factory());
        host.setArtwork (makeArtwork (juce::Colours::green));
        EffectParams effects;
        effects.scale = 1.1f;
        host.setEffects (effects);
        host.start();
        expect (host.tier() == RenderTier::gl);

        auto& gl = fakes.get (RenderTier::gl);
        gl.fail (FallbackCause::shaderCompileFailed, "ERROR: 0:8: syntax error");
        gl.fail (FallbackCause::contextLost, "a second report is stale"); // ignored: same renderer, same generation

        expect (host.tier() == RenderTier::gl); // deferred to a later message, never inside the renderer's own call
        expect (Pump::until ([&] { return host.tier() == RenderTier::cpu; }, 2000));

        expectEquals (static_cast<int> (host.events().size()), 1);
        expect (host.events()[0].from == RenderTier::gl && host.events()[0].to == RenderTier::cpu);
        expect (host.events()[0].cause == FallbackCause::shaderCompileFailed);
        expect (host.events()[0].detail.contains ("syntax error"));
        expectEquals (gl.detaches, 1);

        auto& cpu = fakes.get (RenderTier::cpu);
        expect (cpu.attaches == 1 && cpu.artwork.isValid());
        expectEquals (cpu.effects.scale, 1.1f);

        // And the tier below fails in turn.
        cpu.fail (FallbackCause::cpuOverBudget, "paint takes 12 ms");
        expect (Pump::until ([&] { return host.tier() == RenderTier::staticImage; }, 2000));
        expectEquals (static_cast<int> (host.events().size()), 2);
        expect (host.events()[1].cause == FallbackCause::cpuOverBudget);
    }

    void testLadderNeverGoesUp()
    {
        beginTest ("The ladder only goes down: a stale failure from a replaced tier changes nothing");

        Fakes fakes;
        fakes.cpuAvailable = true;
        VisualHost host ({}, fakes.factory());
        host.start();
        auto& gl = fakes.get (RenderTier::gl);
        gl.fail (FallbackCause::contextLost, "lost");
        expect (Pump::until ([&] { return host.tier() == RenderTier::cpu; }, 2000));

        gl.fail (FallbackCause::watchdogTimeout, "late report from the old tier");
        Pump::run (100);
        expect (host.tier() == RenderTier::cpu);
        expectEquals (static_cast<int> (host.events().size()), 1);

        host.stop();
        host.start(); // a fresh start begins at the top again
        expect (host.tier() == RenderTier::gl);
    }

    void testLifetime()
    {
        beginTest ("Destroying the host with a failure still queued is safe");

        Fakes fakes;
        {
            VisualHost host ({}, fakes.factory());
            host.start();
            fakes.get (RenderTier::gl).fail (FallbackCause::contextLost, "lost just before shutdown");
        } // the host and its renderer are gone; the queued message must not touch them
        Pump::run (100);
        expectEquals (fakes.get (RenderTier::gl).detaches, 1); // reaching here without a crash is the check
    }

    void testRealStaticTier()
    {
        beginTest ("With OpenGL forced off, the real host lands on the CPU tier and paints the artwork");

        VisualHost host (RenderOverrides::parse ("no-opengl"));
        host.setSize (200, 100);
        host.setArtwork (makeArtwork (juce::Colour (0xffff0000)));
        host.start();

        expect (host.tier() == RenderTier::cpu);
        expectEquals (static_cast<int> (host.events().size()), 1);
        expect (host.events()[0].from == RenderTier::gl && host.events()[0].to == RenderTier::cpu);
        expect (host.events()[0].cause == FallbackCause::disabledByRequest);
        expectEquals (host.tickHz(), 60.0); // the CPU tier is paced by the host's timer

        juce::Image canvas (juce::Image::ARGB, 200, 100, true);
        {
            juce::Graphics g (canvas);
            host.paintEntireComponent (g, false);
        }

        expect (canvas.getPixelAt (100, 50) == juce::Colour (0xffff0000)); // the art, in the middle
        expect (canvas.getPixelAt (10, 50) == juce::Colour (0xff000000));  // letterbox bars: the art is square
        expect (host.status().tier == RenderTier::cpu && host.status().ready);
        expect (! host.isGlSurfaceVisible());

        beginTest ("With the CPU tier forced over budget as well, the host ends on the static tier");

        VisualHost forced (RenderOverrides::parse ("no-opengl,cpu-failure"));
        forced.setSize (200, 100);
        forced.setArtwork (makeArtwork (juce::Colour (0xff00ff00)));
        forced.start();
        expect (forced.tier() == RenderTier::cpu);

        for (int i = 0; i < 31; ++i)
        {
            juce::Graphics g (canvas);
            forced.paintEntireComponent (g, false);
        }

        forced.tick(); // the CPU tier notices and reports; the host swaps on the next message
        expect (Pump::until ([&] { return forced.tier() == RenderTier::staticImage; }, 2000));
        expectEquals (static_cast<int> (forced.events().size()), 2);
        expect (forced.events()[1].from == RenderTier::cpu && forced.events()[1].to == RenderTier::staticImage);
        expect (forced.events()[1].cause == FallbackCause::cpuOverBudget);
        expectEquals (forced.tickHz(), pacing::staticHealthHz);

        {
            juce::Graphics g (canvas);
            forced.paintEntireComponent (g, false);
        }
        expect (canvas.getPixelAt (100, 50) == juce::Colour (0xff00ff00)); // the art is still there
    }

    //==========================================================================
    // Real OpenGL. Needs a GPU and creates a small window of its own, so it only runs when
    // SNDS_RENDER_GL_TESTS=1 (run with --filter "Render host").
    struct Scenario
    {
        const char* switches;
        RenderTier expectedTier;
        FallbackCause expectedCause; // of the first event; none = no event expected
    };

    void runScenario (const Scenario& s)
    {
        VisualHost host (RenderOverrides::parse (s.switches));
        host.setBounds (60, 60, 256, 256);
        host.setArtwork (makeArtwork (juce::Colour (0xff3060c0), 128));

        EffectParams strong; // every effect on at once, so the shader's whole path runs
        strong.scale = 1.05f;
        strong.depth = 1.0f;
        strong.glow = 0.8f;
        strong.texture = 0.6f;
        strong.leftBrightness = 1.0f;
        strong.rightBrightness = 0.5f;
        strong.shift = 0.5f;
        host.setEffects (strong);
        host.addToDesktop (juce::ComponentPeer::windowHasTitleBar);
        host.setVisible (true);
        Pump::run (100);

        host.start();
        Pump::until (
            [&]
            {
                return host.tier() == s.expectedTier && host.status().ready == (s.expectedTier == RenderTier::gl)
                    && (s.expectedTier != RenderTier::gl || host.status().frames > 5);
            },
            8000);
        Pump::run (200);

        const auto status = host.status();
        logMessage (juce::String ("scenario '") + s.switches + "': tier " + tierName (host.tier()) + ", frames "
                    + juce::String (status.frames) + ", gpu "
                    + (status.gpu.contextCreated
                           ? status.gpu.renderer + " / " + status.gpu.version + " / GLSL " + status.gpu.glslVersion
                           : juce::String ("none")));

        for (const auto& e : host.events())
            logMessage ("    " + tierName (e.from) + " -> " + tierName (e.to) + " [" + causeName (e.cause) + "] "
                        + e.detail);

        expect (host.tier() == s.expectedTier, juce::String ("tier for '") + s.switches + "'");

        if (s.expectedCause == FallbackCause::none)
        {
            expect (host.events().empty());
            expect (status.gpu.meetsMinimum && status.gpu.major * 10 + status.gpu.minor >= 32);
            expect (status.frames > 5 && status.attached);
        }
        else
        {
            expect (! host.events().empty() && host.events()[0].cause == s.expectedCause);
        }

        host.stop();
    }

    void testOptInGl()
    {
        beginTest ("Real OpenGL: normal start, then each forced failure falls back (opt-in: SNDS_RENDER_GL_TESTS=1)");

        if (juce::SystemStats::getEnvironmentVariable ("SNDS_RENDER_GL_TESTS", {}) != "1")
        {
            logMessage ("skipped: set SNDS_RENDER_GL_TESTS=1 to run the real OpenGL scenarios");
            return;
        }

        runScenario ({ "", RenderTier::gl, FallbackCause::none });
        runScenario ({ "shader-failure", RenderTier::cpu, FallbackCause::shaderCompileFailed });
        runScenario ({ "require-gl=9.9", RenderTier::cpu, FallbackCause::belowMinimumVersion });
        runScenario ({ "context-loss=600", RenderTier::cpu, FallbackCause::contextLost });
        runScenario ({ "no-opengl", RenderTier::cpu, FallbackCause::disabledByRequest });
    }
};

RenderHostTests renderHostTests;
} // namespace
} // namespace snds::rendering
