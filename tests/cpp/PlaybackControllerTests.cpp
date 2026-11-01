#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "PlaybackTestKit.h"
#include "SongFixtures.h"
#include "TestHelpers.h"
#include "app/AppOptions.h"
#include "app/CacheStore.h"
#include "app/MixService.h"
#include "app/PlaybackController.h"
#include "app/Settings.h"
#include "app/SongPackageEdit.h"
#include "model/SourceFingerprint.h"
#include "ui/TransportBar.h"

#include <cmath>

namespace snds
{
namespace
{
using namespace snds::test;

constexpr int rate = 44100;
constexpr int frames = rate * 2;

// Zero-mean noise stems whose sum is the "original": enough for the alignment check to be real.
float noise (int seed, std::int64_t frame, int channel)
{
    auto x = static_cast<std::uint64_t> (frame) * 0x9E3779B97F4A7C15ULL
           + static_cast<std::uint64_t> (seed) * 0xD1B54A32D192ED03ULL
           + static_cast<std::uint64_t> (channel) * 0x8CB92BA72F3D8DD7ULL;
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return static_cast<float> (static_cast<double> (x >> 11) / 9007199254740992.0) - 0.5f;
}

float stemNoise (int stem, std::int64_t frame, int channel)
{
    return 0.2f * noise (stem + 1, frame, channel);
}

float mixture (std::int64_t frame, int channel)
{
    return ((stemNoise (0, frame, channel) + stemNoise (1, frame, channel)) + stemNoise (2, frame, channel))
         + stemNoise (3, frame, channel);
}

void writeWav (const juce::File& file,
               int channels,
               std::int64_t length,
               const std::function<float (std::int64_t, int)>& sample)
{
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    auto writer = wav.createWriterFor (
        stream,
        juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (channels).withBitsPerSample (32));
    juce::AudioBuffer<float> buffer (channels, static_cast<int> (length));

    for (int channel = 0; channel < channels; ++channel)
        for (int i = 0; i < length; ++i)
            buffer.setSample (channel, i, sample (i, channel));

    writer->writeFromAudioSampleBuffer (buffer, 0, static_cast<int> (length));
}

/** A prepared package on disk (real stems, an original beside it), as the cache would hold it. */
struct Package
{
    explicit Package (float gain = 1.0f)
        : cacheDir (temp.child ("cache")),
          jobsDir (temp.child ("jobs"))
    {
        original = temp.child ("original.wav");
        writeWav (original, 2, frames, [gain] (std::int64_t f, int c) { return gain * mixture (f, c); });
        const auto identity = computeFingerprint (original);
        SongFixture fixture;
        fixture.sha256 = identity.fingerprint.sha256;
        fixture.sizeBytes = identity.fingerprint.sizeBytes;
        key = fixture.key();
        directory = fixture.write (
            cacheDir,
            [] (juce::DynamicObject& root)
            {
                auto* source = root.getProperty ("source").getDynamicObject();
                source->setProperty ("frames", static_cast<juce::int64> (frames));
                source->setProperty ("sampleRate", rate);
                source->setProperty ("durationSeconds", static_cast<double> (frames) / rate);

                for (const auto* name : SongMetadata::stemNames())
                    root.getProperty ("stems").getDynamicObject()->getProperty (name).getDynamicObject()->setProperty (
                        "frames", static_cast<juce::int64> (frames));
            });

        for (int stem = 0; stem < 4; ++stem)
            writeWav (
                directory.getChildFile (juce::String (SongMetadata::stemNames()[static_cast<size_t> (stem)]) + ".wav"),
                2,
                frames,
                [stem, gain] (std::int64_t f, int c) { return gain * stemNoise (stem, f, c); });
    }

    OpenSong open (bool originalAvailable = true) const
    {
        OpenSong song;
        song.entry = *CacheStore (cacheDir, jobsDir).findEntry (key);
        song.originalFile = original;
        song.originalAvailable = originalAvailable;
        song.originalNote = originalAvailable ? juce::String() : "The original (original.wav) was not found.";
        return song;
    }

    ScopedTempDirectory temp;
    juce::File cacheDir, jobsDir, directory, original;
    juce::String key;
};

struct Rig
{
    Rig()
        : controller (engine, nullptr, queue.dispatcher())
    {
        engine.prepare (44100.0, 512);
    }

    bool pumpUntilState (PlaybackState state, int timeoutMs = 30000)
    {
        return queue.pumpUntil ([&] { return controller.view().state == state; }, timeoutMs);
    }

    DispatchQueue queue;
    audio::PlaybackEngine engine;
    PlaybackController controller;
};

class PlaybackControllerTests final : public juce::UnitTest
{
public:
    PlaybackControllerTests()
        : juce::UnitTest ("PlaybackController", "snds")
    {
    }

    void runTest() override
    {
        testOpenAndTransport();
        testOriginalStates();
        testFailures();
        testRelease();
        testClosingFreesFiles();
        testReloadKeepsPosition();
        testWaitsForDevice();
        testNewestSongWins();
        testCancelPreparing();
        testDescriptions();
        testOptions();
    }

private:
    void testOpenAndTransport()
    {
        beginTest ("PlaybackController: a song opens paused, plays, pauses, stops, seeks, loops and switches A/B");
        Package package;
        Rig rig;
        expect (rig.controller.view().state == PlaybackState::unloaded);

        rig.controller.setSong (package.open());
        expect (rig.controller.view().state == PlaybackState::preparing);
        expect (rig.pumpUntilState (PlaybackState::ready), "ready");
        auto view = rig.controller.view();
        expect (rig.engine.isLoaded() && ! rig.engine.isPlaying(), "loaded, not playing");
        expect (view.original.isAvailable(), "the original was verified: " + view.original.reason);
        expectWithinAbsoluteError (view.durationSeconds, static_cast<double> (frames) / rate, 1.0e-6);
        expectEquals (rig.controller.loadedKey(), package.key);

        rig.controller.play();
        expect (rig.controller.view().state == PlaybackState::playing);
        const auto rendered = renderOffline (rig.engine, 20000, 512);
        expect (std::any_of (rendered.left.begin(), rendered.left.end(), [] (float v) { return v != 0.0f; }),
                "sound is produced");

        rig.controller.pause();
        renderOffline (rig.engine, 2000, 512);
        view = rig.controller.view();
        expect (view.state == PlaybackState::paused && view.positionSeconds > 0.3);

        rig.controller.stop();
        view = rig.controller.view();
        expect (view.state == PlaybackState::ready && view.positionSeconds == 0.0);

        rig.controller.seekToFraction (0.5);
        expectWithinAbsoluteError (rig.controller.view().positionSeconds, 0.5 * frames / rate, 0.001);
        expect (rig.controller.view().state == PlaybackState::paused);

        rig.controller.setLooping (true);
        expect (rig.controller.view().looping && rig.engine.isLooping());

        rig.controller.setOriginalSelected (true);
        expect (rig.controller.view().originalSelected);
        rig.controller.setOriginalSelected (false);
        expect (! rig.controller.view().originalSelected);

        rig.controller.setSong (std::nullopt);
        expect (rig.controller.view().state == PlaybackState::unloaded && ! rig.engine.isLoaded());
    }

    void testOriginalStates()
    {
        beginTest ("PlaybackController: a missing original disables A/B and says why");
        {
            Package package;
            Rig rig;
            rig.controller.setSong (package.open (false));
            expect (rig.pumpUntilState (PlaybackState::ready));
            expect (rig.controller.view().original.kind == OriginalAvailability::Kind::missing);
            expect (! rig.engine.hasOriginal());
            rig.controller.setOriginalSelected (true);
            expect (! rig.controller.view().originalSelected);
            expect (TransportBar::describeOriginal (rig.controller.view().original).contains ("Locate"));
            rig.controller.play(); // the stems still play
            expect (rig.controller.view().state == PlaybackState::playing);
        }

        beginTest ("PlaybackController: an original removed after discovery leaves stems playable");
        {
            Package package;
            auto song = package.open();
            expect (package.original.deleteFile());
            Rig rig;
            rig.controller.setSong (song);
            expect (rig.pumpUntilState (PlaybackState::ready));
            expect (rig.controller.view().original.kind == OriginalAvailability::Kind::unusable);
            expect (rig.controller.view().original.reason.isNotEmpty());
            expect (! rig.engine.hasOriginal());
            rig.controller.play();
            expect (rig.controller.view().state == PlaybackState::playing);
        }

        beginTest ("PlaybackController: a same-size replacement original is refused with the reason");
        {
            Package package;
            const auto originalSize = package.original.getSize();
            writeWav (package.original, 2, frames, [] (std::int64_t f, int c) { return 0.3f * noise (77, f, c); });
            expectEquals (package.original.getSize(), originalSize);
            Rig rig;
            rig.controller.setSong (package.open());
            expect (rig.pumpUntilState (PlaybackState::ready));
            const auto original = rig.controller.view().original;
            expect (original.kind == OriginalAvailability::Kind::unusable);
            expect (original.reason.contains ("does not match"), original.reason);
            expect (TransportBar::describeOriginal (original).contains ("does not match"));
            expect (! rig.engine.hasOriginal());
            rig.controller.setOriginalSelected (true);
            expect (! rig.controller.view().originalSelected);
            rig.controller.play();
            expect (rig.controller.view().state == PlaybackState::playing);
        }

        beginTest ("PlaybackController: a small same-size edit is refused even when audio still aligns");
        {
            Package package;
            const auto originalSize = package.original.getSize();
            writeWav (package.original,
                      2,
                      frames,
                      [] (std::int64_t f, int c) { return mixture (f, c) + (f == frames - 1 ? 0.001f : 0.0f); });
            expectEquals (package.original.getSize(), originalSize);
            Rig rig;
            rig.controller.setSong (package.open());
            expect (rig.pumpUntilState (PlaybackState::ready));
            expect (rig.controller.view().original.reason.contains ("cached source content"));
            expect (! rig.engine.hasOriginal());
        }

        beginTest ("PlaybackController: an original offset from the stems is refused, not played misaligned");
        {
            Package package;
            writeWav (package.original,
                      2,
                      frames,
                      [] (std::int64_t f, int c) { return f < 250 ? 0.0f : mixture (f - 250, c); });
            Rig rig;
            auto song = package.open();
            // This offset is part of the cached source, so identity passes and alignment decides.
            const auto identity = computeFingerprint (package.original);
            song.entry.song->source.sha256 = identity.fingerprint.sha256;
            song.entry.song->source.sizeBytes = identity.fingerprint.sizeBytes;
            rig.controller.setSong (song);
            expect (rig.pumpUntilState (PlaybackState::ready));
            expect (rig.controller.view().original.kind == OriginalAvailability::Kind::unusable);
            expect (rig.controller.view().original.reason.contains ("250 samples"),
                    rig.controller.view().original.reason);
        }
    }

    void testFailures()
    {
        beginTest ("PlaybackController: a damaged stem fails the load with its name, and nothing plays");
        {
            Package package;
            writeWav (package.directory.getChildFile ("drums.wav"),
                      2,
                      frames - 1000,
                      [] (std::int64_t f, int c) { return stemNoise (0, f, c); });
            Rig rig;
            rig.controller.setSong (package.open());
            expect (rig.pumpUntilState (PlaybackState::failed));
            const auto view = rig.controller.view();
            expect (view.message.contains ("drums.wav") && view.message.contains ("frames"), view.message);
            expect (! rig.engine.isLoaded() && ! view.canPlay());
            expect (TransportBar::describeState (view).startsWith ("Cannot play"));
            rig.controller.play();
            expect (! rig.engine.isPlaying());
        }

        beginTest ("PlaybackController: an unreadable stem file fails the load");
        {
            Package package;
            package.directory.getChildFile ("vocals.wav").replaceWithText ("this is not audio");
            Rig rig;
            rig.controller.setSong (package.open());
            expect (rig.pumpUntilState (PlaybackState::failed));
            expect (rig.controller.view().message.contains ("vocals.wav"));
        }
    }

    void testRelease()
    {
        beginTest ("PlaybackController: releasing a song frees its files for deletion");
        Package package;
        Rig rig;
        rig.controller.setSong (package.open());
        expect (rig.pumpUntilState (PlaybackState::ready));

        CacheStore store (package.cacheDir, package.jobsDir);
        const auto lockedDelete =
            store.deleteEntry ("not-the-song"); // different key: a no-op that must not release anything
        (void) lockedDelete;
        rig.controller.releaseSong ("some-other-key");
        expect (rig.engine.isLoaded(), "another key leaves the song alone");

        rig.controller.releaseSong (package.key);
        expect (! rig.engine.isLoaded());
        expect (rig.controller.view().state == PlaybackState::unloaded);
        const auto removed = store.deleteEntry (package.key);
        expect (removed.wasOk(), removed.getErrorMessage());
        expect (! package.directory.exists(), "the folder is gone");
    }

    void testClosingFreesFiles()
    {
        beginTest ("PlaybackController: closing the song (as the cache view does) frees its files for deletion");
        Package package;
        Rig rig;
        rig.controller.setSong (package.open());
        expect (rig.pumpUntilState (PlaybackState::ready));
        rig.controller.play();

        rig.controller.setSong (std::nullopt);
        expect (! rig.engine.isLoaded());

        CacheStore store (package.cacheDir, package.jobsDir);
        const auto removed = store.deleteEntry (package.key);
        expect (removed.wasOk(), removed.getErrorMessage());
        expect (! package.directory.exists());
    }

    void testReloadKeepsPosition()
    {
        beginTest ("PlaybackController: locating the original reloads the song at the same position, paused");
        Package package;
        Rig rig;
        rig.controller.setSong (package.open (false));
        expect (rig.pumpUntilState (PlaybackState::ready));
        rig.controller.seekToFraction (1.0 / 3.0);
        const auto before = rig.engine.getPositionFrames();
        expect (before > 20000);

        auto located = package.open (true); // the same song, now with its original
        located.originalJustLocated = true;
        rig.controller.setSong (located);
        expect (rig.queue.pumpUntil ([&] { return rig.controller.view().original.isAvailable(); }));
        expect (rig.pumpUntilState (PlaybackState::paused), "paused at the old position");
        expectEquals (static_cast<juce::int64> (rig.engine.getPositionFrames()), static_cast<juce::int64> (before));
        expect (rig.engine.hasOriginal() && ! rig.engine.isPlaying());
    }

    void testWaitsForDevice()
    {
        beginTest ("PlaybackController: a song prepared before any audio device exists loads when one starts");
        Package package;
        DispatchQueue queue;
        audio::PlaybackEngine engine; // never prepared: no device yet
        PlaybackController controller (engine, nullptr, queue.dispatcher());
        controller.setSong (package.open());
        expect (queue.pumpUntil ([&] { return controller.view().original.isAvailable(); }));
        auto view = controller.view();
        expect (view.state == PlaybackState::preparing && view.message.contains ("audio device"), view.message);
        expect (! engine.isLoaded());
        expect (! view.canPlay());

        engine.prepare (48000.0, 512); // the device came up
        controller.deviceStatusChanged();
        expect (controller.view().state == PlaybackState::ready);
        expect (engine.isLoaded() && engine.hasOriginal());
        expectEquals (engine.getDeviceSampleRate(), 48000.0);
    }

    void testNewestSongWins()
    {
        beginTest ("PlaybackController: opening another song while one is preparing keeps only the newest");
        Package first (0.9f);
        Package second (0.8f);
        Rig rig;
        rig.controller.setSong (first.open());
        rig.controller.setSong (second.open());
        expect (rig.pumpUntilState (PlaybackState::ready));
        rig.queue.runQueued();
        expectEquals (rig.controller.loadedKey(), second.key);
        expectEquals (rig.controller.view().state == PlaybackState::ready ? 1 : 0, 1);

        // Replacing a loaded song unloads it at once, releasing its files.
        rig.controller.setSong (first.open());
        expect (! rig.engine.isLoaded(), "the old song is unloaded while the new one prepares");
        expect (rig.pumpUntilState (PlaybackState::ready));
        expect (second.directory.deleteRecursively(), "the replaced song files are free");
    }

    void testCancelPreparing()
    {
        beginTest ("PlaybackController: closing during preparation suppresses the pending result");
        Package package;
        Rig rig;
        rig.controller.setSong (package.open());
        rig.controller.setSong (std::nullopt);
        rig.queue.runQueued();
        expect (rig.controller.view().state == PlaybackState::unloaded);
        expect (! rig.engine.isLoaded() && ! rig.engine.hasOriginal());
    }

    void testDescriptions()
    {
        beginTest ("PlaybackController: the transport text reflects each state and the device");
        PlaybackView view;
        view.device.state = audio::DeviceStatus::State::running;
        expectEquals (TransportBar::describeState (view), juce::String ("No song loaded."));

        view.state = PlaybackState::preparing;
        expectEquals (TransportBar::describeState (view), juce::String ("Preparing audio..."));
        view.message = "Waiting for an audio device.";
        expectEquals (TransportBar::describeState (view), juce::String ("Waiting for an audio device."));

        view.state = PlaybackState::failed;
        view.message = "drums.wav cannot be read.";
        expectEquals (TransportBar::describeState (view), juce::String ("Cannot play: drums.wav cannot be read."));

        view.message = {};
        view.state = PlaybackState::ready;
        expectEquals (TransportBar::describeState (view), juce::String ("Ready"));
        view.state = PlaybackState::paused;
        expectEquals (TransportBar::describeState (view), juce::String ("Paused"));
        view.state = PlaybackState::playing;
        expectEquals (TransportBar::describeState (view), juce::String ("Playing"));
        view.buffering = true;
        expectEquals (TransportBar::describeState (view), juce::String ("Buffering..."));
        view.buffering = false;
        view.underruns = 2;
        expectEquals (TransportBar::describeState (view), juce::String ("Playing - 2 dropouts"));

        view.underruns = 0;
        view.state = PlaybackState::paused;
        view.device.state = audio::DeviceStatus::State::fallback;
        view.device.deviceName = "Speakers";
        view.device.preferredDeviceName = "Headphones";
        expect (TransportBar::describeState (view).contains ("on Speakers (Headphones is not available)"));
        expect (view.canPlay(), "a fallback device is usable");

        view.device.state = audio::DeviceStatus::State::lost;
        view.device.message = "Headphones is not available.";
        const auto lost = TransportBar::describeState (view);
        expect (lost.startsWith ("Audio device unavailable") && lost.contains ("position is kept"), lost);
        expect (! view.canPlay() && view.canSeek(), "seeking still works without a device");

        OriginalAvailability availability;
        availability.kind = OriginalAvailability::Kind::checking;
        expect (TransportBar::describeOriginal (availability).contains ("Checking"));
        availability.kind = OriginalAvailability::Kind::unusable;
        availability.reason = "offset by 5 samples";
        expect (TransportBar::describeOriginal (availability).contains ("offset by 5 samples"));
        availability.kind = OriginalAvailability::Kind::available;
        expect (TransportBar::describeOriginal (availability).isEmpty());
    }

    void testOptions()
    {
        beginTest ("PlaybackController: --no-audio and --open-song parse");
        const auto options = AppOptions::parse ({ "--no-audio", "--open-song", "abc-123" });
        expect (options.noAudio && options.openSong == "abc-123" && options.problems.isEmpty());
        expect (! AppOptions::parse ({ "--open-song" }).problems.isEmpty(), "a key is required");
        expect (! AppOptions::parse ({}).noAudio);

        const auto cwd = juce::File::getCurrentWorkingDirectory();
        const auto exporting = AppOptions::parse ({ "--export-diagnostics", "out/diag.zip" }, cwd);
        expect (exporting.exportDiagnostics == cwd.getChildFile ("out/diag.zip") && exporting.problems.isEmpty());
        expect (! AppOptions::parse ({ "--export-diagnostics" }).problems.isEmpty(), "a file is required");
    }
};

PlaybackControllerTests playbackControllerTests;

//==============================================================================
void disturbMix (dsp::ParameterStore& store)
{
    for (int stem = 0; stem < 4; ++stem)
    {
        const auto s = static_cast<dsp::Stem> (stem);
        store.set (s, dsp::StemControl::gain, -5.0f - static_cast<float> (stem));
        store.set (s, dsp::StemControl::pan, -0.4f);
        store.set (s, dsp::StemControl::lowCut, 250.0f);
        store.set (s, dsp::StemControl::highCut, 6000.0f);
        store.set (s, dsp::StemControl::reverbSend, 0.4f);
        store.set (s, dsp::StemControl::delaySend, 0.3f);
    }

    store.set (dsp::Stem::drums, dsp::StemControl::mute, 1.0f);
    store.set (dsp::Stem::vocals, dsp::StemControl::solo, 1.0f);
    store.set (dsp::GlobalControl::reverbReturn, 0.4f);
    store.set (dsp::GlobalControl::delayReturn, 0.4f);
    store.set (dsp::GlobalControl::delayFeedback, 0.5f);
    store.set (dsp::GlobalControl::delayDivision, 3.0f);
    store.set (dsp::GlobalControl::masterGain, -4.0f);
    store.set (dsp::GlobalControl::tempoBpm, 71.0f);
}

bool isDefaultMix (const dsp::ParameterStore& store, float expectedBpm)
{
    for (int i = 0; i < dsp::numParameters; ++i)
    {
        const auto expected =
            i == dsp::parameterIndex (dsp::GlobalControl::tempoBpm) ? expectedBpm : dsp::parameterSpec (i).defaultValue;

        if (std::abs (store.get (i) - expected) > 1.0e-4f)
            return false;
    }

    return true;
}

struct MixRig
{
    MixRig()
        : controller (engine, nullptr, queue.dispatcher(), &mix)
    {
        engine.setMixStageFactory (mix.stageFactory());
        engine.prepare (44100.0, 512);
    }

    bool pumpUntilState (PlaybackState state)
    {
        return queue.pumpUntil ([&] { return controller.view().state == state; });
    }

    DispatchQueue queue;
    MixService mix;
    audio::PlaybackEngine engine;
    PlaybackController controller;
};

class EphemeralMixTests final : public juce::UnitTest
{
public:
    EphemeralMixTests()
        : juce::UnitTest ("EphemeralMix", "snds")
    {
    }

    void runTest() override
    {
        testNewSongStartsFromDefaults();
        testReopeningStartsOver();
        testLocatingKeepsTheMix();
        testNothingIsPersisted();
    }

private:
    void testNewSongStartsFromDefaults()
    {
        beginTest ("EphemeralMix: every opened song starts from the default mix, at its own tempo");
        Package first (0.9f);
        Package second (0.8f);
        expect (writeBpmOverride (second.directory, SongMetadata::BpmOverride { 93.0, "numeric" }).wasOk());
        MixRig rig;
        auto& store = rig.mix.parameters();

        disturbMix (store);
        rig.controller.setSong (first.open());
        expect (isDefaultMix (store, 120.0f), "the first song resets what was set before it");
        expect (rig.pumpUntilState (PlaybackState::ready));
        const auto firstProcessor = rig.mix.processor();
        expect (firstProcessor != nullptr);

        disturbMix (store);
        rig.controller.setSong (second.open());
        expect (isDefaultMix (store, 93.0f), "the next song gets defaults and its own (user-set) tempo");
        expect (rig.pumpUntilState (PlaybackState::ready));
        expect (rig.mix.processor() != firstProcessor, "a fresh processor, with no effect tails or filter state");
        expect (isDefaultMix (store, 93.0f), "loading does not disturb the defaults");

        disturbMix (store);
        rig.controller.setSong (std::nullopt);
        disturbMix (store);
        rig.controller.setSong (first.open());
        expect (isDefaultMix (store, 120.0f), "after closing a song too");
    }

    void testReopeningStartsOver()
    {
        beginTest ("EphemeralMix: opening the same song again starts over from the beginning with the default mix");
        Package package;
        MixRig rig;
        rig.controller.setSong (package.open());
        expect (rig.pumpUntilState (PlaybackState::ready));
        rig.controller.seekToFraction (0.5);
        disturbMix (rig.mix.parameters());
        expect (rig.engine.getPositionFrames() > 0);

        rig.controller.setSong (package.open()); // from the library again: not a locate
        expect (isDefaultMix (rig.mix.parameters(), 120.0f));
        expect (rig.pumpUntilState (PlaybackState::ready));
        expectEquals (static_cast<juce::int64> (rig.engine.getPositionFrames()), static_cast<juce::int64> (0));
    }

    void testLocatingKeepsTheMix()
    {
        beginTest ("EphemeralMix: locating the original of the open song keeps the mix and the playhead");
        Package package;
        MixRig rig;
        rig.controller.setSong (package.open (false));
        expect (rig.pumpUntilState (PlaybackState::ready));
        rig.controller.seekToFraction (0.5);
        disturbMix (rig.mix.parameters());
        const auto position = rig.engine.getPositionFrames();

        auto located = package.open (true);
        located.originalJustLocated = true;
        rig.controller.setSong (located);
        expectEquals (rig.mix.parameters().get (dsp::parameterIndex (dsp::GlobalControl::masterGain)), -4.0f);
        expectEquals (rig.mix.parameters().get (dsp::parameterIndex (dsp::GlobalControl::tempoBpm)), 71.0f);
        expect (rig.queue.pumpUntil ([&] { return rig.controller.view().original.isAvailable(); }));
        expect (rig.pumpUntilState (PlaybackState::paused));
        expectEquals (static_cast<juce::int64> (rig.engine.getPositionFrames()), static_cast<juce::int64> (position));
        expectEquals (rig.mix.parameters().get (dsp::parameterIndex (dsp::Stem::drums, dsp::StemControl::mute)), 1.0f);
    }

    void testNothingIsPersisted()
    {
        beginTest ("EphemeralMix: mix state never reaches settings.json, the song package, or any other file");
        ScopedTempDirectory home;
        Package package;
        const auto songJson = package.directory.getChildFile ("song.json");
        const auto songBefore = songJson.loadFileAsString();

        Settings settings (home.child ("settings.json"));
        settings.setForceCpu (true);
        settings.setAudioDeviceState ("<DEVICESETUP deviceType=\"Fake\" audioOutputDeviceName=\"X\"/>");

        MixRig rig;
        rig.controller.setSong (package.open());
        expect (rig.pumpUntilState (PlaybackState::ready));
        rig.engine.play();
        disturbMix (rig.mix.parameters());
        renderOffline (rig.engine, 20000, 512);
        rig.controller.setOriginalSelected (true);
        rig.controller.setLooping (true);
        rig.controller.stop();
        expect (settings.save().wasOk());

        // settings.json holds only what it always did.
        const auto parsed = juce::JSON::parse (home.child ("settings.json").loadFileAsString());
        juce::StringArray keys;

        if (auto* object = parsed.getDynamicObject())
            for (const auto& property : object->getProperties())
                keys.add (property.name.toString());

        keys.sort (false);
        expectEquals (keys.joinIntoString (","), juce::String ("audioDevice,forceCpu,format,version"));

        // The package is unchanged by mixing (only a tempo choice rewrites it, and only its override).
        expectEquals (songJson.loadFileAsString(), songBefore);

        // No parameter id appears in any text file anywhere under the data folder or the package.
        juce::StringArray ids;

        for (int i = 0; i < dsp::numParameters; ++i)
            ids.add (juce::String (dsp::parameterSpec (i).id.view().data(), dsp::parameterSpec (i).id.view().size()));

        for (const auto& root : { home.get(), package.temp.get() })
            for (const auto& entry : juce::RangedDirectoryIterator (root, true, "*", juce::File::findFiles))
            {
                if (entry.getFile().hasFileExtension ("wav"))
                    continue;

                const auto text = entry.getFile().loadFileAsString();

                for (const auto& id : ids)
                    expect (! text.contains (id), entry.getFile().getFileName() + " mentions " + id);
            }
    }
};

EphemeralMixTests ephemeralMixTests;
} // namespace
} // namespace snds
