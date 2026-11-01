#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "TestHelpers.h"
#include "app/SongSession.h"

#include <cmath>

namespace snds
{
namespace
{
using State = SessionState;

/** A short 16-bit WAV: a sine of the given frequency, so two files can have equal sizes and
    different content. */
juce::File writeWav (const juce::File& file, double frequency, double seconds = 0.5, int channels = 2)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();

    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    auto writer = format.createWriterFor (
        stream,
        juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (channels).withBitsPerSample (16));

    const auto frames = static_cast<int> (44100.0 * seconds);
    juce::AudioBuffer<float> buffer (channels, frames);

    for (int channel = 0; channel < channels; ++channel)
        for (int i = 0; i < frames; ++i)
            buffer.setSample (channel, i, 0.3f * std::sin (static_cast<float> (6.2831853 * frequency * i / 44100.0)));

    writer->writeFromAudioSampleBuffer (buffer, 0, frames);
    writer.reset();
    return file;
}

class SongSessionTests final : public juce::UnitTest
{
public:
    SongSessionTests()
        : juce::UnitTest ("Song session", "snds")
    {
    }

    void runTest() override
    {
        beginTest ("Setup");
        // The test executable, copied as python.exe, plays the worker (see TestMain.cpp).
        fakeWorker = { workRoot.child ("worker"), "test" };
        fakeWorker.pythonExecutable().getParentDirectory().createDirectory();
        expect (juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                    .copyFileTo (fakeWorker.pythonExecutable()));

        runProgressHelpers();
        runImportFlow();
        runCacheHit();
        runFailures();
        runCancellation();
        runRejections();
        runMissingOriginal();
        runDestruction();
    }

private:
    test::LazyTempDirectory workRoot;
    WorkerLocation fakeWorker;
    int homeCounter = 0;

    /** Everything one session needs, in its own SNDS_HOME. */
    struct Rig
    {
        explicit Rig (SongSessionTests& owner, const juce::String& initialMode)
            : paths { owner.workRoot.child ("home" + juce::String (++owner.homeCounter)) },
              cache (paths),
              originals (paths.home.getChildFile ("originals.json")),
              mode (initialMode)
        {
            environment.paths = paths;
            environment.worker = owner.fakeWorker;
            environment.cache = &cache;
            environment.originals = &originals;
            environment.forceCpu = [this] { return forceCpu; };
            environment.workerEnvironment = [this]
            {
                juce::StringPairArray variables;
                variables.set ("SNDS_FAKE_WORKER", mode);
                return variables;
            };
            environment.dispatcher = queue.dispatcher();
            environment.cancelGraceMs = 400;
            session = std::make_unique<SongSession> (environment);
            session->onStatusChanged = [this] (const SessionStatus& status) { history.push_back (status); };
            session->onSongOpened = [this] (const OpenSong&) { ++opens; };
        }

        bool waitFor (State state, int timeoutMs = 30000)
        {
            return queue.pumpUntil ([&] { return session->status().state == state; }, timeoutMs);
        }

        bool sawState (State state) const
        {
            for (const auto& status : history)
                if (status.state == state)
                    return true;

            return false;
        }

        /** Imports a file and answers the artwork question with the default art. */
        bool importAndPrepare (const juce::File& file)
        {
            session->importSong (file);

            if (! waitFor (State::awaitingArtwork))
                return false;

            session->provideArtwork (makeFallbackChoice (file.getFileName()));
            return queue.pumpUntil ([&] { return ! session->status().isBusy(); }, 60000);
        }

        juce::File jobFolder() const { return paths.jobsDirectory(); }

        DataPaths paths;
        CacheStore cache;
        OriginalLocations originals;
        juce::String mode;
        bool forceCpu = false;
        test::DispatchQueue queue;
        SessionEnvironment environment;
        std::unique_ptr<SongSession> session;
        std::vector<SessionStatus> history;
        int opens = 0;
    };

    //==========================================================================
    void runProgressHelpers()
    {
        beginTest ("Stage weights add up and overall progress only moves forward");
        {
            double total = 0.0;
            double last = -1.0;

            for (int i = 0; i < worker::stageCount; ++i)
            {
                const auto stage = static_cast<worker::Stage> (i);
                total += stageWeight (stage);

                for (const double fraction : { 0.0, 0.5, 1.0 })
                {
                    const auto value = overallProgress (stage, fraction);
                    expect (value >= last, "monotonic");
                    last = value;
                }

                expect (stageDisplayName (stage).isNotEmpty());
            }

            expectWithinAbsoluteError (total, 1.0, 1.0e-9);
            expectWithinAbsoluteError (overallProgress (worker::Stage::finalizing, 1.0), 1.0, 1.0e-9);
            expectWithinAbsoluteError (overallProgress (worker::Stage::decoding, -3.0), 0.0, 1.0e-9);
        }
    }

    //==========================================================================
    void runImportFlow()
    {
        beginTest ("Import: validate, fingerprint, confirm artwork, prepare, open");
        Rig rig (*this, "publish");
        const auto wav = writeWav (workRoot.child ("music\\first.wav"), 440.0);

        rig.session->importSong (wav);
        expect (rig.waitFor (State::awaitingArtwork));
        expect (rig.sawState (State::checking));
        expectEquals (rig.session->status().songName, juce::String ("first.wav"));
        expect (! rig.session->status().detectedArtwork.has_value(), "a plain WAV has no embedded art");
        expect (rig.session->status().canCancel);
        expect (rig.jobFolder().getNumberOfChildFiles (juce::File::findDirectories) == 0,
                "no job before the artwork choice");

        rig.session->provideArtwork (makeFallbackChoice ("first.wav"));
        expect (rig.session->status().state == State::preparing);
        expect (rig.queue.pumpUntil ([&] { return rig.session->openSong().has_value(); }, 60000));
        expect (rig.queue.pumpUntil ([&] { return ! rig.session->status().isBusy(); }));

        double lastProgress = -1.0;
        bool sawSeparating = false;

        for (const auto& status : rig.history)
        {
            if (status.state != State::preparing)
                continue;

            expect (status.progress >= lastProgress, "progress never goes backwards");
            lastProgress = status.progress;
            sawSeparating = sawSeparating || status.stage.startsWith ("Separating stems (2 of 6)");
            expect (status.device.isEmpty() || status.device == "cuda");
        }

        expect (sawSeparating);
        expect (lastProgress > 0.0);

        expectEquals (rig.opens, 1);
        const auto& opened = rig.session->openSong();
        expect (opened.has_value() && opened->entry.isPlayable());

        if (opened)
        {
            expect (opened->originalAvailable);
            expect (opened->originalFile == wav);
            expectEquals (opened->song().worker.device, juce::String ("cuda"));
            expect (! opened->bpmIsDefault());
            expect (rig.originals.find (opened->entry.key).has_value(), "the original's location is remembered");
        }

        expect (rig.cache.listJobs().empty(), "the finished job folder is deleted");
        expect (rig.session->status().headline.startsWith ("Ready"));
        expect (rig.cache.listPlayableEntries().size() == 1);
        expect (rig.session->status().justPrepared, "a fresh preparation announces itself");
        expect (rig.session->status().detail.contains ("Prepared in"), rig.session->status().detail);
    }

    void runCacheHit()
    {
        beginTest ("Opening a song that is already cached skips artwork and the worker");
        Rig rig (*this, "publish");
        const auto wav = writeWav (workRoot.child ("music\\cached.wav"), 330.0);
        expect (rig.importAndPrepare (wav));
        expectEquals (rig.opens, 1);

        // Same cache, new session, and a worker that would fail if it ran.
        rig.mode = "fail";
        rig.history.clear();
        rig.session = std::make_unique<SongSession> (rig.environment);
        rig.session->onStatusChanged = [&rig] (const SessionStatus& status) { rig.history.push_back (status); };
        rig.session->onSongOpened = [&rig] (const OpenSong&) { ++rig.opens; };

        rig.session->importSong (wav);
        expect (rig.queue.pumpUntil ([&] { return ! rig.session->status().isBusy(); }));
        expectEquals (rig.opens, 2);
        expect (! rig.sawState (State::awaitingArtwork));
        expect (! rig.sawState (State::preparing));
        expect (rig.session->openSong().has_value() && rig.session->openSong()->originalAvailable);
        expect (! rig.session->status().justPrepared, "a library open is not announced as new");
        expect (rig.session->status().detail.contains ("already prepared"), rig.session->status().detail);

        // Closing the song lets go of it: nothing is open, the cache entry can go, and listeners hear it.
        int closed = 0;
        rig.session->onSongClosed = [&closed] { ++closed; };
        const auto key = rig.session->openSong()->entry.key;
        rig.session->closeSong();
        expect (! rig.session->openSong().has_value() && closed == 1);
        expect (! rig.session->status().isBusy());
        rig.session->closeSong();
        expectEquals (closed, 1, "closing nothing does nothing");
        expect (rig.cache.deleteEntry (key).wasOk());
        expect (rig.cache.listJobs().empty(), "no job folder was created");

        beginTest ("A damaged or outdated cache entry is replaced when the song is imported again");
        {
            Rig second (*this, "publish");
            const auto other = writeWav (workRoot.child ("music\\stale.wav"), 550.0);
            const auto fingerprint = computeFingerprint (other);
            expect (fingerprint.ok());
            const auto stale = second.paths.cacheDirectory().getChildFile (fingerprint.fingerprint.cacheKey());
            stale.createDirectory();
            stale.getChildFile ("song.json").replaceWithText ("{ not json");
            stale.getChildFile ("drums.wav").replaceWithText ("leftover");

            expect (second.importAndPrepare (other));
            expect (second.session->openSong().has_value());
            expect (second.sawState (State::awaitingArtwork), "a replacement needs the artwork decision");
            expect (second.cache.findEntry (fingerprint.fingerprint.cacheKey())->isPlayable());
        }
    }

    //==========================================================================
    void runFailures()
    {
        beginTest ("A CUDA failure offers a retry on the CPU, which then succeeds");
        {
            Rig rig (*this, "cuda-fail");
            const auto wav = writeWav (workRoot.child ("music\\cuda.wav"), 261.0);
            expect (rig.importAndPrepare (wav));

            const auto& status = rig.session->status();
            expect (status.state == State::failed);
            expectEquals (status.failureCode, juce::String ("cuda_failed"));
            expect (status.canRetryOnCpu);
            expect (! status.canRetry);
            expect (status.guidance.containsIgnoreCase ("CPU"), status.guidance);
            expect (! rig.session->openSong().has_value());
            expect (rig.cache.listPlayableEntries().empty());

            rig.mode = "publish";
            rig.session->retryOnCpu();
            expect (rig.queue.pumpUntil ([&] { return ! rig.session->status().isBusy(); }, 60000));
            expect (rig.session->openSong().has_value());

            if (rig.session->openSong())
                expectEquals (rig.session->openSong()->song().worker.device, juce::String ("cpu"));
            expect (rig.cache.listJobs().empty(), "the failed attempt's job folder is gone once the retry succeeds");
        }

        beginTest ("Settings.forceCpu reaches the worker request");
        {
            Rig rig (*this, "publish");
            rig.forceCpu = true;
            expect (rig.importAndPrepare (writeWav (workRoot.child ("music\\forced.wav"), 300.0)));
            expect (rig.session->openSong().has_value());

            if (rig.session->openSong())
                expectEquals (rig.session->openSong()->song().worker.device, juce::String ("cpu"));
        }

        beginTest ("cache_conflict removes the unusable entry and offers to prepare again");
        {
            Rig rig (*this, "conflict");
            const auto wav = writeWav (workRoot.child ("music\\conflict.wav"), 350.0);
            const auto key = computeFingerprint (wav).fingerprint.cacheKey();
            expect (rig.importAndPrepare (wav));

            const auto& status = rig.session->status();
            expect (status.state == State::failed);
            expectEquals (status.failureCode, juce::String ("cache_conflict"));
            expect (status.canRetry);
            expect (status.headline.contains ("removed"), status.headline);
            expect (! rig.cache.findEntry (key).has_value(), "the conflicting entry is gone");

            rig.mode = "publish";
            rig.session->retry();
            expect (rig.queue.pumpUntil ([&] { return ! rig.session->status().isBusy(); }, 60000));
            expect (rig.session->openSong().has_value());
            expect (rig.cache.findEntry (key)->isPlayable());
        }

        beginTest ("A full disk says how to fix it and does not offer a pointless retry");
        {
            Rig rig (*this, "disk-full");
            expect (rig.importAndPrepare (writeWav (workRoot.child ("music\\full.wav"), 280.0)));
            const auto& status = rig.session->status();
            expect (status.state == State::failed && ! status.canRetry && ! status.canRetryOnCpu);
            expect (status.guidance.contains ("Free some disk space"), status.guidance);
        }

        beginTest ("A crashed worker is reported with its stderr and keeps the job folder");
        {
            Rig rig (*this, "crash");
            expect (rig.importAndPrepare (writeWav (workRoot.child ("music\\crash.wav"), 290.0)));
            const auto& status = rig.session->status();
            expect (status.state == State::failed);
            expectEquals (status.failureCode, juce::String ("worker_crashed"));
            expect (status.detail.contains ("exit code"), status.detail);
            expect (status.failureDetails.contains ("fake worker stderr"), status.failureDetails);
            expect (status.canRetry);
            expect (rig.cache.listJobs().size() == 1, "kept for diagnosis");
            expect (! rig.cache.listJobs()[0].active, "but no longer owned by a running job");
            expect (rig.cache.listPlayableEntries().empty());
        }

        beginTest ("A worker that cannot be started points at the bootstrap script");
        {
            Rig rig (*this, "publish");
            rig.environment.worker = { workRoot.child ("no-worker"), "test" };
            rig.session = std::make_unique<SongSession> (rig.environment);
            rig.session->onStatusChanged = [&rig] (const SessionStatus& status) { rig.history.push_back (status); };
            expect (rig.importAndPrepare (writeWav (workRoot.child ("music\\nostart.wav"), 270.0)));
            expectEquals (rig.session->status().failureCode, juce::String ("launch_failed"));
            expect (rig.session->status().guidance.contains ("bootstrap.ps1"));
        }
    }

    //==========================================================================
    void runCancellation()
    {
        beginTest ("Cancel during separation leaves no partial song and no job folder");
        {
            Rig rig (*this, "cooperative");
            const auto wav = writeWav (workRoot.child ("music\\cancel.wav"), 200.0);
            rig.session->importSong (wav);
            expect (rig.waitFor (State::awaitingArtwork));
            rig.session->provideArtwork (makeFallbackChoice ("cancel"));
            expect (rig.queue.pumpUntil ([&] { return rig.session->status().stage.startsWith ("Reading the audio"); }));
            expect (rig.session->status().canCancel);

            rig.session->cancel();
            expect (rig.session->status().state == State::cancelling);
            expect (! rig.session->status().canCancel);
            expect (rig.queue.pumpUntil ([&] { return ! rig.session->status().isBusy(); }));

            expect (rig.session->status().state == State::idle);
            expect (rig.session->status().headline.startsWith ("Cancelled"), rig.session->status().headline);
            expect (! rig.session->openSong().has_value());
            expect (rig.cache.listEntries().empty(), "nothing in the cache, playable or not");
            expect (rig.cache.listJobs().empty(), "the job folder was deleted");
            expectEquals (rig.opens, 0);
        }

        beginTest ("A worker that ignores cancel is terminated and the result is the same");
        {
            Rig rig (*this, "hang");
            rig.session->importSong (writeWav (workRoot.child ("music\\hang.wav"), 210.0));
            expect (rig.waitFor (State::awaitingArtwork));
            rig.session->provideArtwork (makeFallbackChoice ("hang"));
            expect (rig.queue.pumpUntil ([&] { return rig.session->status().stage.startsWith ("Reading the audio"); }));
            rig.session->cancel();
            expect (rig.queue.pumpUntil ([&] { return ! rig.session->status().isBusy(); }));
            expect (rig.session->status().headline.startsWith ("Cancelled"));
            expect (rig.cache.listEntries().empty() && rig.cache.listJobs().empty());
        }

        beginTest ("Cancel while reading the file and while confirming artwork");
        {
            Rig rig (*this, "publish");
            rig.session->importSong (writeWav (workRoot.child ("music\\early.wav"), 220.0));
            rig.session->cancel();
            expect (rig.queue.pumpUntil ([&] { return ! rig.session->status().isBusy(); }));
            expect (rig.session->status().headline.startsWith ("Import cancelled")
                    || rig.session->status().state == State::idle);
            expect (! rig.session->openSong().has_value());

            rig.session->importSong (writeWav (workRoot.child ("music\\early2.wav"), 230.0));
            expect (rig.waitFor (State::awaitingArtwork));
            rig.session->cancel();
            expect (rig.session->status().state == State::idle);
            expect (rig.cache.listJobs().empty());
        }
    }

    //==========================================================================
    void runRejections()
    {
        beginTest ("Unusable files fail with the reason and never reach the worker");
        {
            Rig rig (*this, "fail");

            const auto text = workRoot.child ("music\\notes.txt");
            text.getParentDirectory().createDirectory();
            text.replaceWithText ("hello");
            rig.session->importSong (text);
            expect (rig.queue.pumpUntil ([&] { return rig.session->status().state == State::failed; }));
            expectEquals (rig.session->status().failureCode, juce::String ("import_rejected"));
            expect (rig.session->status().detail.contains ("WAV and MP3"));

            rig.session->dismissFailure();
            expect (rig.session->status().state == State::idle);

            rig.session->importSong (writeWav (workRoot.child ("music\\surround.wav"), 100.0, 0.2, 6));
            expect (rig.queue.pumpUntil ([&] { return rig.session->status().state == State::failed; }));
            expect (rig.session->status().detail.contains ("6 channels"), rig.session->status().detail);

            rig.session->importSong (workRoot.child ("music\\gone.wav"));
            expect (rig.queue.pumpUntil ([&] { return rig.session->status().state == State::failed; }));
            expect (rig.session->status().detail.contains ("not found"));

            expect (rig.cache.listJobs().empty() && rig.cache.listEntries().empty());
        }
    }

    //==========================================================================
    void runMissingOriginal()
    {
        beginTest ("A song whose original is gone still opens; Locate Original accepts only the real file");
        Rig rig (*this, "publish");
        const auto wav = writeWav (workRoot.child ("music\\locate.wav"), 247.0);
        expect (rig.importAndPrepare (wav));
        const auto key = rig.session->openSong()->entry.key;

        const auto moved = workRoot.child ("elsewhere\\renamed copy.wav");
        moved.getParentDirectory().createDirectory();
        expect (wav.moveFileTo (moved));

        const auto reopen = [&]
        {
            rig.session = std::make_unique<SongSession> (rig.environment);
            rig.session->onStatusChanged = [&rig] (const SessionStatus& status) { rig.history.push_back (status); };
            rig.session->onSongOpened = [&rig] (const OpenSong&) { ++rig.opens; };
        };

        reopen();
        rig.session->openCached (key);
        expect (rig.session->openSong().has_value(), "the stems open without the original");

        if (! rig.session->openSong())
            return;

        expect (rig.session->openSong()->entry.isPlayable());
        expect (! rig.session->openSong()->originalAvailable);
        expect (rig.session->openSong()->originalNote.contains ("not found"), rig.session->openSong()->originalNote);

        // A file with the same length but other content: refused after hashing.
        const auto impostor = writeWav (workRoot.child ("elsewhere\\impostor.wav"), 880.0);
        expectEquals (impostor.getSize(), moved.getSize());
        rig.session->locateOriginal (impostor);
        expect (rig.queue.pumpUntil ([&] { return rig.session->status().state == State::failed; }));
        expectEquals (rig.session->status().failureCode, juce::String ("locate_mismatch"));
        expect (! rig.session->openSong()->originalAvailable);

        // A file of another size: refused without hashing.
        rig.session->dismissFailure();
        rig.session->locateOriginal (writeWav (workRoot.child ("elsewhere\\longer.wav"), 247.0, 1.0));
        expect (rig.session->status().state == State::failed);
        expectEquals (rig.session->status().failureCode, juce::String ("locate_mismatch"));
        expect (! rig.session->openSong()->originalAvailable);

        // The real original, renamed and moved: accepted by its content.
        rig.session->dismissFailure();
        const auto opensBefore = rig.opens;
        rig.session->locateOriginal (moved);
        expect (rig.session->status().state == State::locating);
        expect (rig.queue.pumpUntil ([&] { return rig.session->openSong()->originalAvailable; }));
        expect (rig.session->openSong()->originalFile == moved);
        expectEquals (rig.opens, opensBefore + 1);
        expect (rig.session->status().headline.startsWith ("Original located"));

        // The location survives a restart.
        OriginalLocations reloaded (rig.paths.home.getChildFile ("originals.json"));
        expect (reloaded.find (key).has_value() && *reloaded.find (key) == moved);
        reopen();
        rig.session->openCached (key);
        expect (rig.session->openSong() && rig.session->openSong()->originalAvailable);

        beginTest ("A cache entry that is gone or unusable cannot be opened");
        rig.session->openCached ("0000-1");
        expect (rig.session->status().state == State::failed);
        expectEquals (rig.session->status().failureCode, juce::String ("cache_missing"));
    }

    //==========================================================================
    void runDestruction()
    {
        beginTest ("Destroying the session mid-prepare kills the worker and delivers nothing later");
        {
            Rig rig (*this, "hang");
            rig.session->importSong (writeWav (workRoot.child ("music\\destroy.wav"), 190.0));
            expect (rig.waitFor (State::awaitingArtwork));
            rig.session->provideArtwork (makeFallbackChoice ("destroy"));
            expect (rig.queue.pumpUntil ([&] { return rig.session->status().stage.startsWith ("Reading the audio"); }));

            const auto before = juce::Time::getMillisecondCounterHiRes();
            const auto calls = rig.history.size();
            rig.session.reset();
            expect (juce::Time::getMillisecondCounterHiRes() - before < 15000.0);

            rig.queue.pumpUntil ([] { return false; }, 100);
            expect (rig.history.size() == calls, "no status change after destruction");
        }
    }
};

SongSessionTests songSessionTests;
} // namespace
} // namespace snds
