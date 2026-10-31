// Phase 4 validation through the real engine and the real UI controls: a parameter storm while
// playing and seeking, the solo/mute truth table driven from the stem cards, and (opt-in) images of
// the stem cards. The DSP classes have their own tests (Dsp*Tests); this file proves the pieces
// work together the way the app uses them.
//
// SNDS_HEAVY_TESTS=1 runs the full workloads. SNDS_SNAPSHOT_DIR=<folder> writes the images.

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "DspHeavy.h"
#include "FakeAudioDevice.h"
#include "PlaybackTestKit.h"
#include "app/MixService.h"
#include "audio/Resampler.h"
#include "dsp/MasterStage.h"
#include "dsp/StemLevelStage.h"
#include "ui/MixPanel.h"

#include <atomic>
#include <cmath>
#include <numbers>
#include <thread>

// The allocation counter AudioCallbackAuditTests.cpp installs for the whole test executable.
namespace snds::test::audit
{
extern thread_local bool tracking;
extern thread_local long long allocationCount;
} // namespace snds::test::audit

namespace snds
{
namespace
{
using namespace snds::test;

constexpr int rate = 44100;

/** Counts this thread's heap allocations while alive. */
struct AllocationScope
{
    AllocationScope() noexcept
        : wasTracking (audit::tracking),
          start (audit::allocationCount)
    {
        audit::tracking = true;
    }

    ~AllocationScope() { audit::tracking = wasTracking; }

    long long allocations() const noexcept { return audit::allocationCount - start; }

    bool wasTracking;
    long long start;
};

audio::PlaybackSources loudToneSources (std::int64_t frames, float amplitude)
{
    audio::PlaybackSources sources;
    sources.sampleRate = rate;
    sources.lengthFrames = frames;

    for (int stem = 0; stem < 4; ++stem)
        sources.stems[static_cast<size_t> (stem)] = std::make_unique<SyntheticSource> (
            rate,
            frames,
            [stem, amplitude] (std::int64_t f, int c)
            {
                return amplitude
                     * static_cast<float> (std::sin (2.0 * std::numbers::pi * (220.0 * (stem + 1) + 7.0 * c)
                                                     * static_cast<double> (f) / rate));
            });

    return sources;
}

//==============================================================================
class ParameterStormTests final : public juce::UnitTest
{
public:
    ParameterStormTests()
        : juce::UnitTest ("Phase4Stress", "snds")
    {
    }

    void runTest() override
    {
        beginTest (
            "Phase4Stress: every control hammered from another thread while playing, seeking, pausing and looping");
        MixService mix;
        audio::PlaybackEngine engine;
        engine.setMixStageFactory (mix.stageFactory());
        engine.prepare (48000.0, 512); // resampled: the whole callback path, not just the mix
        const std::int64_t frames = std::int64_t { rate } * 20;
        expect (engine.load (loudToneSources (frames, 0.9f)));
        engine.play();

        std::atomic<bool> stop { false };
        std::atomic<long long> writes { 0 };

        std::thread writer (
            [&]
            {
                juce::Random random (17);

                while (! stop.load())
                {
                    const auto index = random.nextInt (dsp::numParameters);
                    const auto& spec = dsp::parameterSpec (index);
                    const int style = random.nextInt (6);
                    float value = spec.minimum + random.nextFloat() * (spec.maximum - spec.minimum);

                    if (style == 0)
                        value = spec.minimum;
                    else if (style == 1)
                        value = spec.maximum;

                    mix.parameters().set (index, value);

                    if (random.nextInt (400) == 0)
                        mix.resetMix (20.0 + random.nextDouble() * 380.0);

                    writes.fetch_add (1);

                    if (random.nextInt (8) == 0)
                        std::this_thread::yield();
                }
            });

        juce::Random random (4);
        std::vector<float> left (1024), right (1024);
        float* channels[2] = { left.data(), right.data() };
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + workload (1500.0, 9000.0);
        double peak = 0.0;
        long long blocks = 0;
        long long allocations = 0;
        bool finite = true;
        bool metersSane = true;

        while (juce::Time::getMillisecondCounterHiRes() < deadline)
        {
            switch (random.nextInt (9))
            {
                case 0:
                    engine.seek (static_cast<std::int64_t> (random.nextDouble() * static_cast<double> (frames)));
                    break;
                case 1:
                    engine.seek (frames - random.nextInt (3000));
                    break;
                case 2:
                    engine.pause();
                    break;
                case 3:
                    engine.play();
                    break;
                case 4:
                    engine.setLooping (random.nextBool());
                    break;
                case 5:
                    engine.stop();
                    engine.play();
                    break;
                default:
                    break;
            }

            const int block = 64 + random.nextInt (900);
            engine.waitForReadAhead (block, 20000);

            {
                AllocationScope scope;
                engine.process (channels, 2, block);
                allocations += scope.allocations();
            }

            ++blocks;

            for (int i = 0; i < block; ++i)
            {
                const auto l = left[static_cast<size_t> (i)];
                const auto r = right[static_cast<size_t> (i)];
                finite = finite && std::isfinite (l) && std::isfinite (r);
                peak = std::max ({ peak, static_cast<double> (std::abs (l)), static_cast<double> (std::abs (r)) });
            }

            dsp::MeterSnapshot meters;

            if (mix.readMeters (meters))
            {
                metersSane = metersSane && std::isfinite (meters.masterPeak) && meters.limiterReductionDb >= 0.0f
                          && meters.masterPeak <= dsp::limiterCeiling * 1.001f;

                for (const auto& stem : meters.stems)
                    metersSane = metersSane && std::isfinite (stem.peak) && std::isfinite (stem.rms)
                              && stem.rms <= stem.peak * 1.001f + 1.0e-6f;
            }
        }

        stop = true;
        writer.join();

        logMessage (juce::String (writes.load()) + " parameter writes while " + juce::String (blocks)
                    + " blocks played; output peak " + juce::String (peak, 3) + " (limiter ceiling "
                    + juce::String (dsp::limiterCeiling, 3) + ")");
        expect (writes.load() > 1000, "the writer really ran");
        expect (finite, "no NaN or infinity reached the output");
        expect (peak <= static_cast<double> (dsp::limiterCeiling), "peak " + juce::String (peak, 6));
        expect (metersSane, "meters stayed finite, ordered and under the ceiling");
        expectEquals (static_cast<juce::int64> (allocations),
                      static_cast<juce::int64> (0),
                      "heap allocations inside process() during the storm");
        expectEquals (engine.getUnderrunCount(), 0);
    }
};

ParameterStormTests parameterStormTests;

//==============================================================================
class DeviceOutputCeilingTests final : public juce::UnitTest
{
public:
    DeviceOutputCeilingTests()
        : juce::UnitTest ("Device output ceiling", "snds")
    {
    }

    void runTest() override
    {
        beginTest ("A bounded polarity step overshoots in the converter but stays limited in engine output");
        constexpr int frames = 8192;
        std::vector<float> input (frames), left (9000), right (9000);
        for (int i = 0; i < frames; ++i)
            input[static_cast<size_t> (i)] = step (i, 0);

        audio::Resampler converter;
        converter.prepare (rate, 48000.0, frames);
        const float* rawInput[2] = { input.data(), input.data() };
        float* rawOutput[2] = { left.data(), right.data() };
        converter.push (rawInput, frames);
        const auto produced = converter.pull (rawOutput, static_cast<int> (left.size()));
        float rawPeak = 0.0f;
        for (int i = 0; i < produced; ++i)
            rawPeak = std::max (rawPeak, std::abs (left[static_cast<size_t> (i)]));
        expect (rawPeak > 1.0f, "the fixture must reproduce interpolation overshoot before limiting");

        for (const bool original : { false, true })
        {
            audio::PlaybackEngine engine;
            engine.prepare (48000.0, 512);
            expect (engine.load (sources (frames)));
            expect (engine.setOriginalSelected (original));
            engine.play();
            check (renderOffline (engine, expectedOutputFrames (frames, rate, 48000.0), 113));
        }

        beginTest ("The ceiling holds across A/B changes, seeks, rate changes and mono output without allocations");
        audio::PlaybackEngine engine;
        engine.prepare (44100.0, 512);
        expect (engine.load (sources (rate * 2)));
        engine.play();
        for (const double deviceRate : { 44100.0, 48000.0, 32000.0, 96000.0 })
        {
            engine.prepare (deviceRate, 512);
            for (const bool original : { true, false })
            {
                expect (engine.setOriginalSelected (original));
                check (renderOffline (engine, 4096, 257));
                engine.seek (1024);
                check (renderOffline (engine, 4096, 257));
            }
        }
        float mono[512] {};
        float* monoOutput[1] = { mono };
        expect (engine.waitForReadAhead (512, 20000));
        long long allocations;
        {
            const AllocationScope tracking;
            engine.process (monoOutput, 1, 512);
            allocations = tracking.allocations();
        }
        expectEquals (allocations, 0LL, "final limiting must not allocate in a mono callback");
        expect (std::all_of (std::begin (mono),
                             std::end (mono),
                             [] (float value)
                             { return std::isfinite (value) && std::abs (value) <= dsp::limiterCeiling; }));
        expectEquals (engine.getUnderrunCount(), 0);
    }

private:
    static float step (std::int64_t frame, int channel)
    {
        const auto value = (frame / 1024) % 2 == 0 ? -dsp::limiterCeiling : dsp::limiterCeiling;
        return channel == 0 ? value : value * 0.25f;
    }

    static audio::PlaybackSources sources (std::int64_t frames)
    {
        audio::PlaybackSources result;
        result.sampleRate = rate;
        result.lengthFrames = frames;
        for (int stem = 0; stem < audio::stemCount; ++stem)
            result.stems[static_cast<size_t> (stem)] = std::make_unique<SyntheticSource> (
                rate,
                frames,
                [stem] (std::int64_t frame, int channel) { return stem == 0 ? step (frame, channel) : 0.0f; });
        result.original = std::make_unique<SyntheticSource> (
            rate, frames, [] (std::int64_t frame, int channel) { return -step (frame, channel); });
        return result;
    }

    void check (const Rendered& rendered)
    {
        expect (! rendered.timedOut && rendered.size() > 0);
        bool bounded = true;
        float stereoError = 0.0f;
        float peak = 0.0f;
        for (size_t i = 0; i < rendered.left.size(); ++i)
        {
            const auto l = rendered.left[i];
            const auto r = rendered.right[i];
            bounded = bounded && std::isfinite (l) && std::isfinite (r) && std::abs (l) <= dsp::limiterCeiling
                   && std::abs (r) <= dsp::limiterCeiling;
            stereoError = std::max (stereoError, std::abs (r - l * 0.25f));
            peak = std::max (peak, std::abs (l));
        }
        expect (bounded, "final device samples must stay finite and at or below -0.3 dBFS");
        expect (peak > 0.9f, "the ceiling must not be satisfied by silence");
        expect (stereoError < 1.0e-7f, "limiting must preserve the linked stereo ratio");
    }
};

DeviceOutputCeilingTests deviceOutputCeilingTests;

//==============================================================================
class UiSoloTruthTableTests final : public juce::UnitTest
{
public:
    UiSoloTruthTableTests()
        : juce::UnitTest ("Phase4SoloUi", "snds")
    {
    }

    void runTest() override
    {
        MessageLoop loop;
        beginTest (
            "Phase4SoloUi: solo and mute pressed on the stem cards give the documented truth table in the real output");
        MixService mix;
        audio::PlaybackEngine engine;
        engine.setMixStageFactory (mix.stageFactory());
        engine.prepare (static_cast<double> (rate), 512);

        // Four constant stems with different levels: the output level says exactly which are audible.
        const float levels[4] = { 0.01f, 0.02f, 0.04f, 0.08f };
        audio::PlaybackSources sources;
        sources.sampleRate = rate;
        sources.lengthFrames = std::int64_t { rate } * 60;

        for (int stem = 0; stem < 4; ++stem)
            sources.stems[static_cast<size_t> (stem)] = std::make_unique<SyntheticSource> (
                rate, sources.lengthFrames, [level = levels[stem]] (std::int64_t, int) { return level; });

        expect (engine.load (std::move (sources)));
        MixPanel panel (mix);
        panel.setSize (1100, panel.getPreferredHeight());
        panel.refresh();
        expect (panel.isEnabled());
        engine.play();
        renderOffline (engine, 4000, 512);

        // Every solo pattern with no mute, each single mute, and all four muted; everything when heavy.
        std::vector<std::pair<int, int>> combos; // {mute bits, solo bits}

        for (int solo = 0; solo < 16; ++solo)
            for (int mute = 0; mute < 16; ++mute)
                if (heavyTests() || mute == 0 || mute == 15 || (mute & (mute - 1)) == 0)
                    combos.emplace_back (mute, solo);

        int mismatches = 0;
        auto press = [&] (ParameterToggle& button, bool wanted)
        {
            if (button.getToggleState() != wanted)
                button.keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
        };

        for (const auto& [muteBits, soloBits] : combos)
        {
            std::array<bool, 4> mute {}, solo {};

            for (int stem = 0; stem < 4; ++stem)
            {
                mute[static_cast<size_t> (stem)] = (muteBits >> stem) & 1;
                solo[static_cast<size_t> (stem)] = (soloBits >> stem) & 1;
                press (panel.stemCards()[static_cast<size_t> (stem)]->muteButton(), mute[static_cast<size_t> (stem)]);
                press (panel.stemCards()[static_cast<size_t> (stem)]->soloButton(), solo[static_cast<size_t> (stem)]);
            }

            const auto audible = dsp::stemAudibility (mute, solo);
            float expected = 0.0f;

            for (int stem = 0; stem < 4; ++stem)
                expected += audible[static_cast<size_t> (stem)] ? levels[stem] : 0.0f;

            const auto rendered = renderOffline (engine, 3600, 512);
            const auto actual = rendered.left.back();

            if (std::abs (actual - expected) > 3.0e-4f)
            {
                ++mismatches;
                expect (false,
                        "mute " + juce::String (muteBits) + " solo " + juce::String (soloBits) + ": expected "
                            + juce::String (expected, 4) + ", heard " + juce::String (actual, 4));
            }
        }

        expectEquals (mismatches, 0);
        logMessage (juce::String (static_cast<int> (combos.size()))
                    + " mute/solo combinations pressed on the cards matched the truth table");

        // Reset mix clears every solo and mute at once, and the sound comes back.
        panel.resetMix();

        for (const auto& card : panel.stemCards())
            expect (! card->muteButton().getToggleState() && ! card->soloButton().getToggleState());

        const auto all = renderOffline (engine, 3600, 512).left.back();
        expectWithinAbsoluteError (all, 0.15f, 3.0e-4f, "all four stems audible after Reset mix");
    }
};

UiSoloTruthTableTests uiSoloTruthTableTests;

//==============================================================================
/** Opt-in: renders the mix panel to PNG files in three states (SNDS_SNAPSHOT_DIR). */
class MixSnapshotTests final : public juce::UnitTest
{
public:
    MixSnapshotTests()
        : juce::UnitTest ("Phase4Snapshots", "snds")
    {
    }

    void runTest() override
    {
        beginTest (
            "Phase4Snapshots: stem cards default, expanded and with the limiter working (opt-in: SNDS_SNAPSHOT_DIR)");
        const auto directory = juce::SystemStats::getEnvironmentVariable ("SNDS_SNAPSHOT_DIR", {});

        if (directory.isEmpty())
            return;

        MessageLoop loop;

        // The app runs on JUCE's dark scheme; a bare test process starts on a light one.
        juce::LookAndFeel_V4 darkLook (juce::LookAndFeel_V4::getDarkColourScheme());
        juce::LookAndFeel::setDefaultLookAndFeel (&darkLook);
        struct Restore
        {
            ~Restore() { juce::LookAndFeel::setDefaultLookAndFeel (nullptr); }
        } restore;

        MixService mix;
        audio::PlaybackEngine engine;
        engine.setMixStageFactory (mix.stageFactory());
        engine.prepare (static_cast<double> (rate), 512);
        expect (engine.load (loudToneSources (std::int64_t { rate } * 30, 0.25f)));
        engine.play();

        MixPanel panel (mix);
        panel.setSize (1100, panel.getPreferredHeight());
        TempoFxSection::TempoInfo info;
        info.detected = 143.2;
        panel.tempoFx().setTempoInfo (info);

        // 1. Defaults, with the music playing so the meters move.
        renderOffline (engine, 30000, 512);
        panel.refresh();
        save (panel, juce::File (directory).getChildFile ("mix-default.png"));

        // 2. Two cards expanded, a mix with some character.
        mix.parameters().set (dsp::Stem::drums, dsp::StemControl::gain, -4.5f);
        mix.parameters().set (dsp::Stem::bass, dsp::StemControl::lowCut, 80.0f);
        mix.parameters().set (dsp::Stem::bass, dsp::StemControl::pan, -0.3f);
        mix.parameters().set (dsp::Stem::vocals, dsp::StemControl::reverbSend, 0.35f);
        mix.parameters().set (dsp::Stem::vocals, dsp::StemControl::highCut, 9000.0f);
        mix.parameters().set (dsp::Stem::other, dsp::StemControl::mute, 1.0f);
        mix.parameters().set (dsp::GlobalControl::delayDivision, 2.0f);
        panel.stemCards()[1]->setExpanded (true);
        panel.stemCards()[3]->setExpanded (true);
        panel.setSize (1100, panel.getPreferredHeight());
        renderOffline (engine, 30000, 512);
        panel.refresh();
        save (panel, juce::File (directory).getChildFile ("mix-expanded.png"));

        // 3. Everything up: the limiter works, lights, and (after a while) says what to do.
        panel.stemCards()[1]->setExpanded (false);
        panel.stemCards()[3]->setExpanded (false);
        panel.setSize (1100, panel.getPreferredHeight());
        mix.resetMix (143.2);

        for (int i = 0; i < 4; ++i)
            mix.parameters().set (static_cast<dsp::Stem> (i), dsp::StemControl::gain, 6.0f);

        mix.parameters().set (dsp::GlobalControl::masterGain, 4.0f);
        renderOffline (engine, 30000, 512);
        panel.refresh();
        dsp::MeterSnapshot meters;

        if (mix.readMeters (meters))
            panel.master().update (
                meters, juce::Time::getMillisecondCounterHiRes() + 5000.0); // lit for a while: the advice shows

        expect (panel.master().isLimiterLit());
        save (panel, juce::File (directory).getChildFile ("mix-limiter.png"));
    }

private:
    void save (juce::Component& component, const juce::File& file)
    {
        const auto snapshot = component.createComponentSnapshot (component.getLocalBounds(), true, 1.0f);
        juce::Image image (juce::Image::RGB, component.getWidth(), component.getHeight(), true);

        {
            juce::Graphics g (image);              // the image is only complete once g is gone
            g.fillAll (juce::Colour (0xff323e44)); // the dark scheme's window background, as in the app
            g.drawImageAt (snapshot, 0, 0);
        }

        juce::PNGImageFormat png;
        file.deleteFile();
        juce::FileOutputStream out (file);
        expect (out.openedOk(), file.getFullPathName());

        if (out.openedOk())
            png.writeImageToStream (image, out);
    }
};

MixSnapshotTests mixSnapshotTests;
} // namespace
} // namespace snds
