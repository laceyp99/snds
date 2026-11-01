#include <juce_gui_basics/juce_gui_basics.h>

#include "app/MixService.h"
#include "rendering/VisualStatus.h"
#include "ui/FullscreenVisuals.h"
#include "ui/SongPanel.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <vector>

namespace snds
{
namespace
{
using namespace rendering;

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

// A renderer that only remembers what it was given.
struct Seen
{
    EffectParams effects;
    int attaches = 0, detaches = 0, effectCalls = 0;
    bool artwork = false;
};

class RecordingRenderer final : public VisualRenderer
{
public:
    RecordingRenderer (RenderTier t, std::shared_ptr<Seen> s)
        : fakeTier (t),
          seen (std::move (s))
    {
    }

    RenderTier tier() const noexcept override { return fakeTier; }
    AttachResult attach (juce::Component&) override
    {
        ++seen->attaches;
        return {};
    }
    void detach() override { ++seen->detaches; }
    void setArtwork (const ArtworkInput& a) override { seen->artwork = a.isValid(); }
    void setEffects (const EffectParams& e) override
    {
        seen->effects = e;
        ++seen->effectCalls;
    }
    void paint (juce::Graphics&) override {}
    void tick() override {}
    RendererStatus status() const override
    {
        RendererStatus s;
        s.tier = fakeTier;
        s.attached = true;
        return s;
    }
    void setFailureCallback (FailureCallback) override {}

    RenderTier fakeTier;
    std::shared_ptr<Seen> seen;
};

VisualHost::RendererFactory recordingFactory (std::shared_ptr<Seen> seen)
{
    return [seen] (RenderTier tier) -> std::unique_ptr<VisualRenderer>
    { return std::make_unique<RecordingRenderer> (tier, seen); };
}

OpenSong makeSong()
{
    OpenSong song;
    song.entry.song = SongMetadata {};
    song.entry.song->source.fileName = "test song.wav";
    return song;
}

// Drives a MixProcessor (through the MixService, as the engine does) with a loud four-stem signal.
struct Feeder
{
    explicit Feeder (audio::StemMixStage& s, bool loudSignal = true)
        : stage (s),
          loud (loudSignal)
    {
        stage.prepare (rate, 1024);
    }

    void block (int frames = 1584)
    {
        std::array<std::vector<float>, 8> channels;
        for (auto& c : channels)
            c.assign (static_cast<std::size_t> (frames), 0.0f);

        if (loud)
        {
            juce::Random random (static_cast<juce::int64> (position));

            for (int i = 0; i < frames; ++i)
            {
                const auto t = static_cast<double> (position + i) / rate;
                const auto hit = i < frames / 3
                                   ? 0.8f * (random.nextFloat() * 2.0f - 1.0f)
                                         * (1.0f - 3.0f * static_cast<float> (i) / static_cast<float> (frames))
                                   : 0.0f;
                const auto at = static_cast<std::size_t> (i);
                channels[0][at] = channels[1][at] = hit;
                channels[2][at] = channels[3][at] =
                    0.3f * static_cast<float> (std::sin (2.0 * std::numbers::pi * 60.0 * t));
                channels[4][at] = channels[5][at] =
                    0.3f * static_cast<float> (std::sin (2.0 * std::numbers::pi * 6000.0 * t));
                channels[6][at] = channels[7][at] =
                    0.3f * static_cast<float> (std::sin (2.0 * std::numbers::pi * 440.0 * t));
            }
        }

        std::array<std::array<const float*, 2>, 4> pointers;
        std::array<const float* const*, 4> stems {};
        for (std::size_t s = 0; s < 4; ++s)
        {
            pointers[s] = { channels[s * 2].data(), channels[s * 2 + 1].data() };
            stems[s] = pointers[s].data();
        }

        std::vector<float> left (static_cast<std::size_t> (frames)), right (static_cast<std::size_t> (frames));
        std::array<float*, 2> out { left.data(), right.data() };
        stage.process (stems, out.data(), frames);
        position += frames;
    }

    audio::StemMixStage& stage;
    bool loud;
    static constexpr double rate = 48000.0;
    std::int64_t position = 0;
};

class VisualPanelTests final : public juce::UnitTest
{
public:
    VisualPanelTests()
        : juce::UnitTest ("Visual panel", "snds")
    {
    }

    void runTest() override
    {
        const Pump pump;
        testStatusText();
        testStartOnce();
        testDrivenByTheMix();
        testOriginalAndControls();
        testOcclusion();
        testFullscreenContent();
        testFullscreenEveryTier();
        testOptInFullscreenWindow();
    }

private:
    void testStatusText()
    {
        beginTest ("The detail line says which renderer runs and, when it is not the GPU, why");

        RendererStatus gl;
        gl.tier = RenderTier::gl;
        gl.gpu.contextCreated = true;
        gl.gpu.major = 3;
        gl.gpu.minor = 2;
        gl.gpu.glslNumber = 1.5;
        gl.gpu.renderer = "Iris Xe";
        gl.gpu.vendor = "Intel";
        gl.averageFrameMs = 14.4;

        const auto onGpu = describeVisuals (RenderTier::gl, gl, {}, false, true);
        expect (onGpu.startsWith ("Visuals: GPU") && onGpu.contains ("OpenGL 3.2") && onGpu.contains ("Iris Xe")
                && onGpu.contains ("69 fps"));
        expect (describeVisuals (RenderTier::gl, gl, {}, true, true).contains ("Original is playing"));
        expect (describeVisuals (RenderTier::gl, gl, {}, false, false).contains ("effects off"));

        auto event = [] (FallbackCause cause)
        {
            FallbackEvent e;
            e.from = RenderTier::gl;
            e.to = RenderTier::cpu;
            e.cause = cause;
            return e;
        };
        FallbackEvent skipped;
        skipped.cause = FallbackCause::unavailable;

        const auto cpu =
            describeVisuals (RenderTier::cpu, {}, { event (FallbackCause::shaderCompileFailed) }, false, true);
        expect (cpu.startsWith ("Visuals: CPU") && cpu.contains ("shader failed to compile"));
        expect (
            describeVisuals (RenderTier::staticImage, {}, { event (FallbackCause::contextLost), skipped }, false, true)
                .contains ("context was lost"));
        expect (describeVisuals (RenderTier::cpu, {}, { skipped }, false, true)
                == "Visuals: CPU (no GPU effects)"); // "not available" is not a reason
        expect (describeVisuals (RenderTier::none, {}, {}, false, true) == "Visuals: starting");

        for (const auto cause : { FallbackCause::disabledByRequest,
                                  FallbackCause::contextCreationFailed,
                                  FallbackCause::belowMinimumVersion,
                                  FallbackCause::shaderCompileFailed,
                                  FallbackCause::resourceSetupFailed,
                                  FallbackCause::contextLost,
                                  FallbackCause::watchdogTimeout,
                                  FallbackCause::cpuOverBudget })
            expect (causeSummary (cause).isNotEmpty() && ! causeSummary (cause).contains ("-"), causeName (cause));
    }

    void testStartOnce()
    {
        beginTest ("The visuals start once, only when there is a song and the panel is on screen");

        {
            // The default policy needs a window: a panel that is not showing never starts the GPU.
            SongPanel panel;
            panel.setSize (900, 340);
            panel.setSong (makeSong());
            panel.tickVisuals();
            expect (! panel.getVisualHost().isStarted());
            expectEquals (panel.getVisualHost().tickHz(), 0.0);
        }

        auto seen = std::make_shared<Seen>();
        SongPanel panel ({}, recordingFactory (seen));
        panel.setSize (900, 340);
        panel.canStartVisuals = [] { return true; };

        panel.tickVisuals();
        expect (! panel.getVisualHost().isStarted()); // no song yet
        expectEquals (seen->attaches, 0);

        panel.setSong (makeSong());
        expect (seen->artwork == false); // not started: the art waits in the host
        panel.tickVisuals();
        expect (panel.getVisualHost().isStarted() && panel.getVisualHost().tier() == RenderTier::gl);
        expectEquals (seen->attaches, 1);
        expect (seen->artwork); // the song's artwork (or the generated fallback) reached the renderer
        expect (panel.getVisualsDetail().startsWith ("Visuals: GPU"));

        for (int i = 0; i < 5; ++i)
            panel.tickVisuals();

        auto second = makeSong();
        second.entry.song->source.fileName = "another.wav";
        panel.setSong (second); // a new song keeps the running renderer
        panel.tickVisuals();
        expectEquals (seen->attaches, 1);
        expectEquals (seen->detaches, 0);

        panel.setSong (std::nullopt);
        expect (panel.getVisualHost().isVisible()
                && panel.getVisualHost().isStarted()); // not torn down (re-attaching is slow)
        expectEquals (seen->detaches, 0);
    }

    void testDrivenByTheMix()
    {
        beginTest ("While playing, the artwork follows the newest processor's analysis; otherwise it settles");

        MixService mix;
        auto stage = mix.stageFactory()();
        Feeder feeder (*stage);

        auto seen = std::make_shared<Seen>();
        SongPanel panel ({}, recordingFactory (seen));
        panel.setSize (900, 340);
        panel.canStartVisuals = [] { return true; };
        panel.setMixService (&mix);
        double fakeNow = 0.0;
        panel.clockMs = [&fakeNow] { return fakeNow; };
        PlaybackView playing;
        playing.state = PlaybackState::playing;
        auto view = playing;
        panel.viewSource = [&view] { return view; };
        panel.setSong (makeSong());

        // Silent audio and paused: nothing moves.
        for (int i = 0; i < 5; ++i)
            panel.tickVisuals();
        expect (seen->effects.isNeutral());

        // Loud music: drums pulse, bass depth, vocal glow and other texture all reach the picture.
        EffectParams peak;
        for (int i = 0; i < 40; ++i)
        {
            feeder.block();
            panel.tickVisuals();
            fakeNow += 33;
            peak.scale = std::max (peak.scale, seen->effects.scale);
            peak.depth = std::max (peak.depth, seen->effects.depth);
            peak.glow = std::max (peak.glow, seen->effects.glow);
            peak.texture = std::max (peak.texture, seen->effects.texture);
        }

        logMessage ("peak effects while playing: scale " + juce::String (peak.scale) + ", depth "
                    + juce::String (peak.depth) + ", glow " + juce::String (peak.glow) + ", texture "
                    + juce::String (peak.texture));
        expect (peak.scale > 1.005f && peak.scale <= 1.025f + 1.0e-4f); // the default half intensity caps it at +2.5 %
        expect (peak.depth > 0.1f && peak.depth <= 0.5f + 1.0e-4f);
        expect (peak.glow > 0.1f && peak.glow <= 0.4f + 1.0e-4f);
        expect (peak.texture > 0.1f && peak.texture <= 0.3f + 1.0e-4f);

        // Pausing: no more audio is analysed, so the snapshot would freeze; the panel feeds silence instead.
        view.state = PlaybackState::paused;
        for (int i = 0; i < 55; ++i)
        {
            panel.tickVisuals();
            fakeNow += 33;
        }
        expect (seen->effects.glow < 0.02f && seen->effects.depth < 0.02f && seen->effects.scale < 1.002f);

        // Playing again, loud, until the picture is moving...
        view.state = PlaybackState::playing;
        for (int i = 0; i < 20; ++i)
        {
            feeder.block();
            panel.tickVisuals();
            fakeNow += 33;
        }
        expect (seen->effects.glow > 0.1f);

        // ... then a newly loaded song gets a new (silent) processor: the panel reads that one, not the old one.
        auto nextStage = mix.stageFactory()();
        Feeder silent (*nextStage, false);
        for (int i = 0; i < 45; ++i)
        {
            silent.block();
            panel.tickVisuals();
            fakeNow += 33;
        }
        expect (seen->effects.glow < 0.02f && seen->effects.scale < 1.002f);

        // No song: no processor, no crash, still silence.
        mix.endSong();
        for (int i = 0; i < 5; ++i)
            panel.tickVisuals();
        expect (seen->effects.glow < 0.02f);
    }

    void testOriginalAndControls()
    {
        beginTest ("The Original makes the artwork static at once; intensity and effects-on are session settings");

        MixService mix;
        auto stage = mix.stageFactory()();
        Feeder feeder (*stage);
        auto seen = std::make_shared<Seen>();
        SongPanel panel ({}, recordingFactory (seen));
        panel.setSize (900, 340);
        panel.canStartVisuals = [] { return true; };
        panel.setMixService (&mix);
        double fakeNow = 0.0;
        panel.clockMs = [&fakeNow] { return fakeNow; };
        PlaybackView view;
        view.state = PlaybackState::playing;
        panel.viewSource = [&view] { return view; };
        panel.setSong (makeSong());

        for (int i = 0; i < 25; ++i)
        {
            feeder.block();
            panel.tickVisuals();
            fakeNow += 33;
        }
        expect (! seen->effects.isNeutral());
        expect (! panel.getVisualHost().isStaticMode());

        view.originalSelected = true;
        feeder.block();
        panel.tickVisuals();
        expect (panel.getVisualHost().isStaticMode());
        expect (seen->effects.isNeutral()); // while the music is still loud
        for (int i = 0; i < 20; ++i)
        {
            feeder.block();
            panel.tickVisuals();
        }
        expect (seen->effects.isNeutral());
        expect (panel.getVisualsDetail().contains ("Original is playing"));

        view.originalSelected = false;
        for (int i = 0; i < 12; ++i)
        {
            feeder.block();
            panel.tickVisuals();
            fakeNow += 33;
        }
        expect (! panel.getVisualHost().isStaticMode() && ! seen->effects.isNeutral());

        // The controls reach the controller.
        auto& visuals = panel.getVisualController();
        expectWithinAbsoluteError (visuals.getSettings().intensity, 0.5f, 1.0e-6f); // the conservative default
        expect (visuals.getSettings().effectsEnabled);

        panel.intensityControl().setValue (80.0, juce::sendNotificationSync);
        expectWithinAbsoluteError (visuals.getSettings().intensity, 0.8f, 1.0e-6f);
        click (panel.effectsControl()); // a click on the toggle turns the effects off
        expect (! visuals.getSettings().effectsEnabled);
        for (int i = 0; i < 80; ++i)
        {
            feeder.block();
            panel.tickVisuals();
            fakeNow += 33;
        }
        expect (seen->effects.scale < 1.002f && seen->effects.glow < 0.02f && seen->effects.depth < 0.02f
                && seen->effects.texture < 0.02f); // effects off: back to the plain artwork, and the intensity is kept
        expectWithinAbsoluteError (visuals.getSettings().intensity, 0.8f, 1.0e-6f);

        // Resetting the mix is not about the picture: the visual settings are not mix state.
        mix.resetMix (120.0);
        expectWithinAbsoluteError (visuals.getSettings().intensity, 0.8f, 1.0e-6f);
        expect (! visuals.getSettings().effectsEnabled);
    }

    void testOcclusion()
    {
        beginTest ("A visuals host that is scrolled out of view or half covered is moved aside, not detached");

        auto seen = std::make_shared<Seen>();
        juce::Component viewportLike;
        viewportLike.setSize (900, 200);
        SongPanel panel ({}, recordingFactory (seen));
        panel.setSize (900, 340);
        panel.canStartVisuals = [] { return true; };
        viewportLike.addAndMakeVisible (panel);
        panel.setTopLeftPosition (0, 0);
        panel.setSong (makeSong());
        panel.tickVisuals();
        expect (! panel.getVisualHost().isOccluded()); // the 160 px of visuals fit in a 200 px viewport

        panel.setTopLeftPosition (0, -60); // scrolled so the top of the artwork is cut off
        panel.tickVisuals();
        expect (panel.getVisualHost().isOccluded());
        expectEquals (seen->detaches, 0);

        panel.setTopLeftPosition (0, 0);
        panel.tickVisuals();
        expect (! panel.getVisualHost().isOccluded());
        expectEquals (seen->attaches, 1);
    }

    //==========================================================================
    // A click, delivered synchronously (Button::triggerClick posts a message).
    static void click (juce::Button& button)
    {
        if (button.getClickingTogglesState())
            button.setToggleState (! button.getToggleState(), juce::dontSendNotification);

        if (button.onClick)
            button.onClick();
    }

    struct Fake
    {
        std::vector<double> seeks;
        int toggles = 0;
        PlaybackView view;
    };

    static FullscreenActions actionsFor (Fake& fake)
    {
        FullscreenActions actions;
        actions.view = [&fake] { return fake.view; };
        actions.togglePlay = [&fake] { ++fake.toggles; };
        actions.seekToFraction = [&fake] (double f) { fake.seeks.push_back (f); };
        return actions;
    }

    void testFullscreenContent()
    {
        beginTest ("Fullscreen: the visuals fill the middle, the bars never cover them, exit is obvious and keys work");

        auto seen = std::make_shared<Seen>();
        VisualHost host ({}, recordingFactory (seen));
        host.start();
        VisualController visuals (host);
        Fake fake;
        fake.view.state = PlaybackState::paused;
        fake.view.durationSeconds = 200.0;
        fake.view.positionSeconds = 100.0;
        fake.view.fraction = 0.5;

        juce::Component parent;
        parent.setSize (1280, 720);
        auto content = std::make_unique<FullscreenVisualsContent> (host, visuals, actionsFor (fake), "My Song");
        int exits = 0;
        content->onExit = [&exits] { ++exits; };
        parent.addAndMakeVisible (*content);
        content->setBounds (parent.getLocalBounds());

        // The host moved here and fills everything between the bars.
        expect (host.getParentComponent() == content.get());
        const auto visualsArea = content->getVisualsArea();
        expect (host.getBounds() == visualsArea);
        expectEquals (visualsArea.getY(), FullscreenVisualsContent::topBarHeight);
        expectEquals (visualsArea.getBottom(), 720 - FullscreenVisualsContent::bottomBarHeight);
        expectEquals (visualsArea.getWidth(), 1280);

        // The exit button is in the top bar, big, enabled and not over the visuals.
        auto& exitButton = content->exitControl();
        expect (exitButton.isVisible() && exitButton.isEnabled());
        expect (exitButton.getBounds().getBottom() <= FullscreenVisualsContent::topBarHeight);
        expect (exitButton.getWidth() >= 150 && exitButton.getRight() <= 1280);
        expect (! exitButton.getBounds().intersects (visualsArea));
        for (auto* control : std::vector<juce::Component*> { &content->playControl(),
                                                             &content->positionControl(),
                                                             &content->effectsControl(),
                                                             &content->intensityControl() })
            expect (! control->getBounds().intersects (visualsArea) && control->isVisible());
        expect (exitButton.getButtonText().contains ("Esc"));

        // Mouse: the button exits.
        click (exitButton);
        expectEquals (exits, 1);

        // Keyboard: Escape exits; transport keys still work.
        expect (content->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)));
        expectEquals (exits, 2);
        expect (content->keyPressed (juce::KeyPress (juce::KeyPress::spaceKey)));
        expectEquals (fake.toggles, 1);
        expect (content->keyPressed (juce::KeyPress (juce::KeyPress::rightKey)));
        expect (content->keyPressed (juce::KeyPress (juce::KeyPress::leftKey)));
        expect (content->keyPressed (juce::KeyPress (juce::KeyPress::homeKey)));
        expectEquals (static_cast<int> (fake.seeks.size()), 3);
        expectWithinAbsoluteError (fake.seeks[0], 0.525, 1.0e-9); // +5 s of 200 s
        expectWithinAbsoluteError (fake.seeks[1], 0.475, 1.0e-9);
        expectEquals (fake.seeks[2], 0.0);
        expect (! content->keyPressed (juce::KeyPress ('q')));

        fake.view.fraction = 0.99;
        content->keyPressed (juce::KeyPress (juce::KeyPress::rightKey));
        expectEquals (fake.seeks.back(), 1.0); // clamped at the end
        fake.view.durationSeconds = 0.0;
        const auto before = fake.seeks.size();
        content->keyPressed (juce::KeyPress (juce::KeyPress::rightKey));
        expectEquals (fake.seeks.size(), before); // nothing to seek in

        // The controls drive the same two settings as the player's panel; the play button follows the state.
        content->effectsControl().setToggleState (false, juce::dontSendNotification);
        click (content->effectsControl());
        expect (visuals.getSettings().effectsEnabled);
        content->intensityControl().setValue (20.0, juce::sendNotificationSync);
        expectWithinAbsoluteError (visuals.getSettings().intensity, 0.2f, 1.0e-6f);
        expectEquals (content->playControl().getButtonText(), juce::String ("Play"));
        content->setStatusText ("Visuals: GPU");

        // Giving the host back leaves it free to return to the player.
        content->releaseHost();
        expect (host.getParentComponent() == nullptr);
        content.reset();
        expect (seen->detaches == 0); // moving the host is not stopping the renderer
    }

    void testFullscreenEveryTier()
    {
        beginTest ("Fullscreen works on every tier (GPU fake, CPU, static): same layout, same exits");

        struct Config
        {
            const char* name;
            const char* switches;
            RenderTier expected;
        };

        for (const auto& config : { Config { "cpu", "no-opengl", RenderTier::cpu },
                                    Config { "static", "no-opengl,cpu-failure", RenderTier::staticImage } })
        {
            VisualHost host (RenderOverrides::parse (config.switches));
            ArtworkInput art;
            art.image = juce::Image (juce::Image::ARGB, 64, 64, true);
            host.setArtwork (art);
            host.setSize (160, 160);
            host.start();

            if (config.expected == RenderTier::staticImage)
            {
                juce::Image canvas (juce::Image::ARGB, 160, 160, true);
                for (int i = 0; i < 31; ++i)
                {
                    juce::Graphics g (canvas);
                    host.paintEntireComponent (g, false);
                }
                host.tick();
                expect (Pump::until ([&] { return host.tier() == RenderTier::staticImage; }, 2000), config.name);
            }

            expect (host.tier() == config.expected, config.name);
            VisualController visuals (host);
            Fake fake;
            juce::Component parent;
            parent.setSize (1920, 1080);
            auto content = std::make_unique<FullscreenVisualsContent> (host, visuals, actionsFor (fake), "Song");
            int exits = 0;
            content->onExit = [&exits] { ++exits; };
            parent.addAndMakeVisible (*content);
            content->setBounds (parent.getLocalBounds());

            expect (host.getParentComponent() == content.get() && ! host.getBounds().isEmpty(), config.name);
            expect (host.tier() == config.expected); // moving the host did not change the tier

            juce::Image canvas (juce::Image::ARGB, 1920, 1080, true);
            {
                juce::Graphics g (canvas);
                host.paintEntireComponent (g, false);
            }

            click (content->exitControl());
            content->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
            expectEquals (exits, 2, config.name);
            content->releaseHost();
        }
    }

    //==========================================================================
    // A real fullscreen window with real OpenGL. It covers the display for a couple of seconds, so it only runs on
    // request: SNDS_RENDER_FULLSCREEN_TESTS=1 (run with --filter "Visual panel").
    void testOptInFullscreenWindow()
    {
        beginTest ("Real fullscreen window: the GPU visuals move into it and back and keep rendering (opt-in)");

        if (juce::SystemStats::getEnvironmentVariable ("SNDS_RENDER_FULLSCREEN_TESTS", {}) != "1")
        {
            logMessage ("skipped: set SNDS_RENDER_FULLSCREEN_TESTS=1 to open a real fullscreen window");
            return;
        }

        SongPanel panel;
        panel.setBounds (80, 80, 900, 340);
        panel.addToDesktop (juce::ComponentPeer::windowHasTitleBar);
        panel.setVisible (true);
        Pump::run (150);
        panel.setSong (makeSong());
        expect (Pump::until (
            [&]
            {
                panel.tickVisuals();
                return panel.getVisualHost().status().frames > 5;
            },
            8000));
        expect (panel.getVisualHost().tier() == RenderTier::gl);

        panel.enterFullscreen();
        expect (panel.isFullscreen());
        auto* window = panel.getFullscreenWindow();
        expect (window != nullptr && panel.getVisualHost().getParentComponent() == &window->getContent());

        const auto framesBefore = panel.getVisualHost().status().frames;
        expect (Pump::until (
            [&]
            {
                panel.tickVisuals();
                return panel.getVisualHost().status().frames > framesBefore + 20;
            },
            8000));
        expect (panel.getVisualHost().tier() == RenderTier::gl);
        logMessage ("fullscreen: gl still rendering, " + panel.getVisualsDetail());

        // Escape, delivered to the screen's own key handler (no input is sent to any window).
        expect (window->getContent().keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)));
        expect (Pump::until ([&] { return ! panel.isFullscreen(); }, 3000));
        expect (panel.getVisualHost().getParentComponent() == &panel);

        const auto framesAfter = panel.getVisualHost().status().frames;
        expect (Pump::until (
            [&]
            {
                panel.tickVisuals();
                return panel.getVisualHost().status().frames > framesAfter + 20;
            },
            8000));
        expect (panel.getVisualHost().tier() == RenderTier::gl);
        expect (panel.getVisualHost().events().empty());
    }
};

VisualPanelTests visualPanelTests;
} // namespace
} // namespace snds
