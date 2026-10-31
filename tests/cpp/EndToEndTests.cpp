#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "TestHelpers.h"
#include "app/Readiness.h"
#include "app/SongSession.h"

#include <cmath>

// Opt-in end-to-end checks of the session against the REAL worker, never run by the normal
// suite (category "snds-e2e"; run `snds_tests --e2e`). They need:
//   SNDS_E2E_HOME   a scratch SNDS_HOME with both checkpoints under models\ (also set as SNDS_HOME)
//   SNDS_E2E_TRACK  an audio file to prepare; it is only read, never changed
// and the worker environment of the checkout. Nothing here touches the real data folder.
namespace snds
{
namespace
{
using State = SessionState;

class CapturingLogger final : public juce::Logger
{
public:
    void logMessage (const juce::String& message) override
    {
        const std::lock_guard<std::mutex> lock (mutex);
        lines.add (message);
    }

    bool contains (const juce::String& text) const
    {
        const std::lock_guard<std::mutex> lock (mutex);

        for (const auto& line : lines)
            if (line.contains (text))
                return true;

        return false;
    }

    juce::String lastLineContaining (const juce::String& text) const
    {
        const std::lock_guard<std::mutex> lock (mutex);

        for (int i = lines.size(); --i >= 0;)
            if (lines[i].contains (text))
                return lines[i];

        return {};
    }

private:
    mutable std::mutex mutex;
    juce::StringArray lines;
};

juce::File writeSine (const juce::File& file, double frequency, double seconds)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    auto writer = format.createWriterFor (
        stream, juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (16));
    const auto frames = static_cast<int> (44100.0 * seconds);
    juce::AudioBuffer<float> buffer (2, frames);

    for (int channel = 0; channel < 2; ++channel)
        for (int i = 0; i < frames; ++i)
            buffer.setSample (channel, i, 0.3f * std::sin (static_cast<float> (6.2831853 * frequency * i / 44100.0)));

    writer->writeFromAudioSampleBuffer (buffer, 0, frames);
    return file;
}

/** The first `seconds` of an audio file as a 16-bit WAV in a scratch folder. */
juce::File writeExcerpt (const juce::File& source, const juce::File& target, double seconds)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (source));

    if (reader == nullptr)
        return {};

    target.getParentDirectory().createDirectory();
    target.deleteFile();
    const auto frames = static_cast<int> (
        juce::jmin<juce::int64> (reader->lengthInSamples, static_cast<juce::int64> (seconds * reader->sampleRate)));
    juce::AudioBuffer<float> buffer (2, frames);
    buffer.clear();
    reader->read (&buffer, 0, frames, 0, true, true);

    if (reader->numChannels == 1)
        buffer.copyFrom (1, 0, buffer, 0, 0, frames);

    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream (target.createOutputStream());
    auto writer = format.createWriterFor (stream,
                                          juce::AudioFormatWriterOptions()
                                              .withSampleRate (reader->sampleRate)
                                              .withNumChannels (2)
                                              .withBitsPerSample (16));
    writer->writeFromAudioSampleBuffer (buffer, 0, frames);
    return target;
}

class EndToEndTests final : public juce::UnitTest
{
public:
    EndToEndTests()
        : juce::UnitTest ("End to end with the real worker", "snds-e2e")
    {
    }

    void runTest() override
    {
        const auto homeText = juce::SystemStats::getEnvironmentVariable ("SNDS_E2E_HOME", {});
        const auto trackText = juce::SystemStats::getEnvironmentVariable ("SNDS_E2E_TRACK", {});

        beginTest ("Setup");
        expect (homeText.isNotEmpty() && trackText.isNotEmpty(), "set SNDS_E2E_HOME and SNDS_E2E_TRACK");

        if (homeText.isEmpty() || trackText.isEmpty())
            return;

        home = juce::File (homeText);
        track = juce::File (trackText);
        expect (track.existsAsFile(), track.getFullPathName());
        expectEquals (juce::SystemStats::getEnvironmentVariable ("SNDS_HOME", {}),
                      homeText,
                      "SNDS_HOME must be the scratch home");
        expect (home.getChildFile ("models\\htdemucs").isDirectory()
                && home.getChildFile ("models\\beat_this").isDirectory());

        juce::Logger::setCurrentLogger (&logger);
        worker = WorkerLocation::fromEnvironment ({});
        const auto readiness = interpretReadiness (runReadinessCommand (worker, 120000), worker);
        logMessage ("Readiness: " + readiness.headline);
        expect (readiness.isReady(), readiness.headline + "\n" + readiness.details);

        if (readiness.isReady())
        {
            prepareMp3Fixture();
            prepareOwnerTrack();
            cancelMidSeparation();
            reopenWithoutReprocessing();
            missingSourceAndLocate();
        }

        juce::Logger::setCurrentLogger (nullptr);
    }

private:
    juce::File home, track;
    WorkerLocation worker;
    CapturingLogger logger;

    struct Rig
    {
        explicit Rig (EndToEndTests& owner, bool forceCpu)
            : paths { owner.home },
              cache (paths),
              originals (paths.home.getChildFile ("originals.json"))
        {
            SessionEnvironment environment;
            environment.paths = paths;
            environment.worker = owner.worker;
            environment.cache = &cache;
            environment.originals = &originals;
            environment.forceCpu = [forceCpu] { return forceCpu; };
            environment.dispatcher = queue.dispatcher();
            environment.cancelGraceMs = 10000;
            session = std::make_unique<SongSession> (environment);
            session->onStatusChanged = [this] (const SessionStatus& status) { history.push_back (status); };
            session->onSongOpened = [this] (const OpenSong&) { ++opens; };
        }

        bool sawState (State state) const
        {
            for (const auto& status : history)
                if (status.state == state)
                    return true;

            return false;
        }

        bool waitIdle (int timeoutMs)
        {
            return queue.pumpUntil ([&] { return ! session->status().isBusy(); }, timeoutMs);
        }

        DataPaths paths;
        CacheStore cache;
        OriginalLocations originals;
        test::DispatchQueue queue;
        std::unique_ptr<SongSession> session;
        std::vector<SessionStatus> history;
        int opens = 0;
    };

    /** Runs a full import with the default artwork; returns seconds from start to idle. */
    double importFully (Rig& rig, const juce::File& file)
    {
        const auto started = juce::Time::getMillisecondCounterHiRes();
        rig.session->importSong (file);
        expect (
            rig.queue.pumpUntil (
                [&]
                { return rig.session->status().state == State::awaitingArtwork || ! rig.session->status().isBusy(); },
                120000),
            "import of " + file.getFileName() + " settles: " + rig.session->status().headline);

        // A song that is already cached opens without the artwork question.
        if (rig.session->status().state == State::awaitingArtwork)
            rig.session->provideArtwork (makeFallbackChoice (file.getFileName()));

        expect (rig.waitIdle (900000), "preparation finishes");
        return (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0;
    }

    void expectOpenedPackage (Rig& rig, const juce::String& what)
    {
        const auto& opened = rig.session->openSong();
        expect (opened.has_value(),
                what + ": a song is open: " + rig.session->status().headline + " " + rig.session->status().detail);

        if (! opened)
            return;

        expect (opened->entry.isPlayable());

        for (const auto* stem : SongMetadata::stemNames())
            expect (opened->entry.directory.getChildFile (juce::String (stem) + ".wav").existsAsFile(), stem);

        const auto& song = opened->song();
        logMessage (what + ": " + song.source.fileName + ", " + juce::String (song.source.durationSeconds, 1) + " s, "
                    + song.worker.device + ", BPM "
                    + (song.bpm.value ? juce::String (*song.bpm.value, 1) : "none (" + song.bpm.error + ")")
                    + ", confidence " + juce::String (song.bpm.confidence, 2));
    }

    void prepareMp3Fixture()
    {
        beginTest ("Prepare the committed MP3 fixture with the real worker");
        const juce::File fixture (juce::String (juce::CharPointer_UTF8 (SNDS_TESTDATA_DIR)).replace ("/", "\\"));
        Rig rig (*this, false);
        const auto seconds = importFully (rig, fixture.getChildFile ("tone-stereo-44100.mp3"));
        logMessage ("MP3 fixture took " + juce::String (seconds, 1) + " s");
        expectOpenedPackage (rig, "mp3");
        expect (rig.sawState (State::preparing));

        if (rig.session->openSong())
            expectEquals (rig.session->openSong()->song().source.container, juce::String ("mp3"));

        expect (rig.cache.listJobs().empty(), "job folder removed after success");
    }

    void prepareOwnerTrack()
    {
        beginTest ("Prepare the owner track with the real worker");
        Rig rig (*this, false);
        const auto seconds = importFully (rig, track);
        logMessage ("Owner track took " + juce::String (seconds, 1) + " s");

        int separatingUpdates = 0;

        for (const auto& status : rig.history)
            if (status.stage.startsWith ("Separating stems"))
                ++separatingUpdates;

        logMessage ("Separating progress updates seen: " + juce::String (separatingUpdates));
        expect (separatingUpdates > 1);
        expectOpenedPackage (rig, "owner track");
        expect (rig.opens == 1);
        expect (rig.cache.listJobs().empty(), "job folder removed after success");
        expect (rig.cache.listPlayableEntries().size() == 2);
    }

    void cancelMidSeparation()
    {
        beginTest ("Cancel during separation leaves no playable partial cache");
        const test::ScopedTempDirectory scratch;
        const auto clip = writeExcerpt (track, scratch.child ("clip.wav"), 45.0);
        expect (clip.existsAsFile());

        Rig rig (*this, true); // CPU: slow enough to cancel inside the separating stage
        const auto before = rig.cache.listEntries().size();
        const auto key = computeFingerprint (clip).fingerprint.cacheKey();

        rig.session->importSong (clip);
        expect (rig.queue.pumpUntil ([&] { return rig.session->status().state == State::awaitingArtwork; }, 120000));
        rig.session->provideArtwork (makeFallbackChoice ("clip"));
        expect (rig.queue.pumpUntil (
                    [&]
                    {
                        return rig.session->status().stage.startsWith ("Separating stems")
                            && rig.session->status().progress > 0.1;
                    },
                    300000),
                "separation is under way: " + rig.session->status().stage);

        const auto mid = rig.session->status().progress;
        rig.session->cancel();
        expect (rig.waitIdle (60000));

        logMessage ("Cancelled at overall progress " + juce::String (mid, 2) + "; "
                    + logger.lastLineContaining ("Worker finished"));
        expect (rig.session->status().headline.startsWith ("Cancelled"), rig.session->status().headline);
        expect (logger.lastLineContaining ("Worker finished").contains ("cancelled")
                || logger.lastLineContaining ("Worker finished").contains ("killed"));
        expect (! rig.cache.findEntry (key).has_value(), "no cache folder for the cancelled song");
        expect (rig.cache.listEntries().size() == before);
        expect (rig.cache.listJobs().empty(), "job folder deleted");
        expect (! rig.session->openSong().has_value());
    }

    void reopenWithoutReprocessing()
    {
        beginTest ("Reopening a prepared song uses the cache only");
        Rig rig (*this, false);
        const auto seconds = importFully (rig, track);
        logMessage ("Reopen took " + juce::String (seconds, 1) + " s");
        expect (rig.session->openSong().has_value());
        expect (! rig.sawState (State::awaitingArtwork) && ! rig.sawState (State::preparing));
        expect (rig.session->openSong() && rig.session->openSong()->originalAvailable);
        expectEquals (rig.opens, 1);
    }

    void missingSourceAndLocate()
    {
        beginTest ("Missing original keeps the stems usable; Locate Original checks the fingerprint");
        const test::ScopedTempDirectory scratch;
        const auto original = writeSine (scratch.child ("moveme\\tone.wav"), 330.0, 6.0);
        juce::String key;

        {
            Rig rig (*this, false);
            importFully (rig, original);
            expectOpenedPackage (rig, "sine");
            key = rig.session->openSong() ? rig.session->openSong()->entry.key : juce::String();
        }

        const auto moved = scratch.child ("elsewhere\\renamed.wav");
        moved.getParentDirectory().createDirectory();
        expect (original.moveFileTo (moved));

        Rig rig (*this, false);
        rig.session->openCached (key);
        expect (rig.session->openSong().has_value());

        if (! rig.session->openSong())
            return;

        expect (rig.session->openSong()->entry.isPlayable());
        expect (! rig.session->openSong()->originalAvailable);
        logMessage ("Missing-original note: " + rig.session->openSong()->originalNote);

        const auto impostor = writeSine (scratch.child ("elsewhere\\impostor.wav"), 523.0, 6.0);
        rig.session->locateOriginal (impostor);
        expect (rig.queue.pumpUntil ([&] { return rig.session->status().state == State::failed; }, 60000));
        expectEquals (rig.session->status().failureCode, juce::String ("locate_mismatch"));
        expect (! rig.session->openSong()->originalAvailable);

        rig.session->dismissFailure();
        rig.session->locateOriginal (moved);
        expect (rig.queue.pumpUntil ([&] { return rig.session->openSong()->originalAvailable; }, 60000));
        expect (rig.session->openSong()->originalFile == moved);

        // Clean up the scratch song so repeated runs start from the same state.
        expect (rig.cache.deleteEntry (key).wasOk());
    }
};

EndToEndTests endToEndTests;
} // namespace
} // namespace snds
