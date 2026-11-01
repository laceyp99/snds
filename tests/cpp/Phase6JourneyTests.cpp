#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "TestHelpers.h"
#include "app/ErrorReport.h"
#include "app/Readiness.h"
#include "app/SongSession.h"
#include "ui/RecentList.h"

#include <functional>

// Opt-in Phase 6 journey checks against the REAL worker (category "snds-e2e"; run
// `snds_tests --e2e`). Same environment as EndToEndTests.cpp:
//   SNDS_E2E_HOME   a scratch SNDS_HOME with both checkpoints under models\ (also set as SNDS_HOME)
//   SNDS_E2E_TRACK  an audio file; only read. Short excerpts of it are made in a scratch folder.
// and, only on a machine with a working NVIDIA GPU:
//   SNDS_E2E_CUDA=1 also run the GPU checks (detection, GPU separation, forced CPU, an injected GPU
//                   failure and the explicit retry on the CPU)
// Nothing here touches the real data folder, and nothing is played.
namespace snds
{
namespace
{
using State = SessionState;

/** The first `seconds` of an audio file as a 16-bit stereo WAV. */
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

/** Same size, different content: the original "edited" in place. */
void changeContent (const juce::File& file)
{
    juce::MemoryBlock bytes;
    file.loadFileAsData (bytes);

    for (size_t i = bytes.getSize() / 2; i < bytes.getSize() / 2 + 2000 && i < bytes.getSize(); ++i)
        bytes[i] = static_cast<char> (bytes[i] ^ 0x55);

    file.replaceWithData (bytes.getData(), bytes.getSize());
}

class Phase6JourneyTests final : public juce::UnitTest
{
public:
    Phase6JourneyTests()
        : juce::UnitTest ("Phase 6 journey with the real worker", "snds-e2e")
    {
    }

    void runTest() override
    {
        const auto homeText = juce::SystemStats::getEnvironmentVariable ("SNDS_E2E_HOME", {});
        const auto trackText = juce::SystemStats::getEnvironmentVariable ("SNDS_E2E_TRACK", {});
        const bool withCuda = juce::SystemStats::getEnvironmentVariable ("SNDS_E2E_CUDA", {}) == "1";

        beginTest ("Setup");
        expect (homeText.isNotEmpty() && trackText.isNotEmpty(), "set SNDS_E2E_HOME and SNDS_E2E_TRACK");

        if (homeText.isEmpty() || trackText.isEmpty())
            return;

        home = juce::File (homeText);
        track = juce::File (trackText);
        expect (track.existsAsFile(), track.getFullPathName());
        expectEquals (juce::SystemStats::getEnvironmentVariable ("SNDS_HOME", {}), homeText);

        worker = WorkerLocation::fromEnvironment ({});
        readiness = interpretReadiness (runReadinessCommand (worker, 120000), worker);
        logMessage ("Readiness: " + readiness.headline + "\n" + readiness.details);
        expect (readiness.isReady(), readiness.headline);

        if (! readiness.isReady())
            return;

        if (withCuda)
            cudaFlow();

        reopenFlow (withCuda);
        cancelAtEachStage();
    }

private:
    juce::File home, track;
    WorkerLocation worker;
    Readiness readiness;
    test::LazyTempDirectory scratch;

    struct Rig
    {
        Rig (Phase6JourneyTests& owner, bool forceCpuNow)
            : paths { owner.home },
              cache (paths),
              originals (paths.home.getChildFile ("originals.json")),
              forceCpu (forceCpuNow)
        {
            SessionEnvironment environment;
            environment.paths = paths;
            environment.worker = owner.worker;
            environment.cache = &cache;
            environment.originals = &originals;
            environment.forceCpu = [this] { return forceCpu; };
            environment.workerEnvironment = [this]
            {
                juce::StringPairArray variables;

                if (injectGpuFailure)
                {
                    // The worker's own test hook: a CUDA error on the first separation segment.
                    variables.set ("SNDS_FAKE_SEPARATOR", "1");
                    variables.set ("SNDS_FAKE_DEVICE", "cuda");
                    variables.set ("SNDS_FAKE_FAIL", "cuda");
                }

                return variables;
            };
            environment.dispatcher = queue.dispatcher();
            environment.cancelGraceMs = 10000;
            session = std::make_unique<SongSession> (environment);
            session->onStatusChanged = [this] (const SessionStatus& status)
            {
                history.push_back (status);

                if (onStatus)
                    onStatus (status);
            };
            session->onSongOpened = [this] (const OpenSong&) { ++opens; };
        }

        bool sawState (State state) const
        {
            for (const auto& status : history)
                if (status.state == state)
                    return true;

            return false;
        }

        bool waitIdle (int timeoutMs = 900000)
        {
            return queue.pumpUntil ([&] { return ! session->status().isBusy(); }, timeoutMs);
        }

        /** Import, answer the artwork question with the default, and wait until nothing is running. */
        bool importFully (const juce::File& file)
        {
            session->importSong (file);

            if (! queue.pumpUntil (
                    [&] { return session->status().state == State::awaitingArtwork || ! session->status().isBusy(); },
                    120000))
                return false;

            if (session->status().state == State::awaitingArtwork)
                session->provideArtwork (makeFallbackChoice (file.getFileName()));

            return waitIdle();
        }

        DataPaths paths;
        CacheStore cache;
        OriginalLocations originals;
        bool forceCpu = false;
        bool injectGpuFailure = false;
        test::DispatchQueue queue;
        std::unique_ptr<SongSession> session;
        std::vector<SessionStatus> history;
        std::function<void (const SessionStatus&)> onStatus;
        int opens = 0;
    };

    juce::File clip (const juce::String& clipName, double seconds)
    {
        return writeExcerpt (track, scratch.child ("music\\" + clipName + ".wav"), seconds);
    }

    void expectOpened (Rig& rig, const juce::String& device, const juce::String& what)
    {
        const auto& opened = rig.session->openSong();
        expect (opened.has_value(), what + ": a song is open: " + rig.session->status().headline);

        if (! opened)
            return;

        expect (opened->entry.isPlayable(), what);
        expectEquals (opened->song().worker.device, device, what + ": device");
        logMessage (what + ": " + juce::String (opened->song().source.durationSeconds, 1) + " s on "
                    + opened->song().worker.device);
    }

    //==========================================================================
    void cudaFlow()
    {
        beginTest ("CUDA: the GPU is detected and separation runs on it");
        expect (readiness.details.containsIgnoreCase ("cuda"), "readiness reports CUDA");
        {
            Rig rig (*this, false);
            expect (rig.importFully (clip ("gpu", 6.0)));
            expectOpened (rig, "cuda", "GPU");
        }

        beginTest ("CUDA: forcing the CPU is honored even though a GPU is available");
        {
            Rig rig (*this, true);
            expect (rig.importFully (clip ("forced-cpu", 7.0)));
            expectOpened (rig, "cpu", "forced CPU");
        }

        beginTest ("CUDA: an injected GPU failure offers Retry on CPU, and the explicit retry succeeds on the CPU");
        {
            Rig rig (*this, false);
            rig.injectGpuFailure = true;
            const auto file = clip ("gpu-fails", 8.0);
            const auto key = computeFingerprint (file).fingerprint.cacheKey();
            expect (rig.importFully (file));

            const auto failed = rig.session->status();
            expect (failed.state == State::failed, failed.headline);
            expectEquals (failed.failureCode, juce::String ("cuda_failed"));
            expect (failed.canRetryOnCpu, "the session offers a retry on the CPU");
            const auto report = reportFromSession (failed);
            expect (report.has_value() && report->hasAction (RecoveryKind::retryOnCpu),
                    "the banner offers Retry on CPU");
            expect (! rig.cache.findEntry (key).has_value(), "no cache entry after the failure");
            expect (! rig.session->openSong().has_value());

            rig.injectGpuFailure = false; // the next worker is the real one
            rig.session->retryOnCpu();
            expect (rig.waitIdle());
            expectOpened (rig, "cpu", "explicit CPU retry");
            expect (rig.cache.findEntry (key).has_value() && rig.cache.findEntry (key)->isPlayable());
            expect (rig.cache.listJobs().empty(), "job folder removed");
        }
    }

    //==========================================================================
    void reopenFlow (bool withCuda)
    {
        const auto original = clip ("reopen", 5.5);
        const auto key = computeFingerprint (original).fingerprint.cacheKey();
        const auto moved = scratch.child ("elsewhere\\renamed.wav");

        beginTest ("Reopen: a prepared song opens from the cache without the worker");
        {
            Rig rig (*this, ! withCuda);
            expect (rig.importFully (original));
            expectOpened (rig, withCuda ? "cuda" : "cpu", "first preparation");
            expect (rig.session->status().justPrepared, "announced as freshly prepared");
        }
        {
            Rig rig (*this, true);
            const auto before = rig.cache.listEntries().size();
            expect (rig.importFully (original));
            expect (rig.session->openSong().has_value());
            expect (! rig.sawState (State::awaitingArtwork) && ! rig.sawState (State::preparing),
                    "no artwork, no worker");
            expect (! rig.session->status().justPrepared);
            expectEquals (static_cast<int> (rig.cache.listEntries().size()), static_cast<int> (before));
            expect (rig.session->openSong()->originalAvailable);

            const auto rows = buildRecentItems (
                { { key, "reopen.wav", original.getFullPathName() } }, { original }, rig.cache.listPlayableEntries());
            expect (rows.size() == 1 && rows[0].state == LibraryItem::State::ready);
        }

        beginTest ("Reopen: with the original moved the song still opens, and Locate accepts only the real file");
        moved.getParentDirectory().createDirectory();
        expect (original.moveFileTo (moved));
        {
            Rig rig (*this, true);
            rig.session->openCached (key);
            expect (rig.session->openSong().has_value() && ! rig.session->openSong()->originalAvailable);

            const auto rows = buildRecentItems (
                { { key, "reopen.wav", original.getFullPathName() } }, { original }, rig.cache.listPlayableEntries());
            expect (rows.size() == 1 && rows[0].state == LibraryItem::State::originalMoved && rows[0].opensFromCache(),
                    "the recent list opens it from the cache and offers Locate");

            // The same file with edited content (same size) is not the original.
            const auto edited = scratch.child ("elsewhere\\edited.wav");
            moved.copyFileTo (edited);
            changeContent (edited);
            rig.session->locateOriginal (edited);
            expect (rig.queue.pumpUntil ([&] { return rig.session->status().state == State::failed; }, 60000));
            expectEquals (
                rig.session->status().failureCode, juce::String ("locate_mismatch"), rig.session->status().headline);
            expect (! rig.session->openSong()->originalAvailable);

            rig.session->dismissFailure();
            rig.session->locateOriginal (moved);
            expect (rig.queue.pumpUntil ([&] { return rig.session->openSong()->originalAvailable; }, 60000));
            expect (rig.session->openSong()->originalFile == moved);
        }

        beginTest ("Reopen: a source whose content changed is a new song, and the old cache is untouched");
        {
            changeContent (moved); // the file at the remembered location is edited in place
            Rig rig (*this, true);
            const auto before = rig.cache.listEntries().size();
            rig.session->importSong (moved);
            expect (rig.queue.pumpUntil ([&] { return rig.session->status().state == State::awaitingArtwork; }, 60000),
                    "it is not a cache hit: " + rig.session->status().headline);
            expect (! rig.session->openSong().has_value(), "nothing opened by itself");

            rig.session->cancel();
            expect (rig.waitIdle (60000));
            expectEquals (static_cast<int> (rig.cache.listEntries().size()), static_cast<int> (before));
            expect (rig.cache.findEntry (key).has_value() && rig.cache.findEntry (key)->isPlayable(),
                    "the entry of the old content is still there");
        }

        expect (rig_cleanup (key));
    }

    bool rig_cleanup (const juce::String& key)
    {
        Rig rig (*this, true);
        return rig.cache.deleteEntry (key).wasOk();
    }

    //==========================================================================
    struct StageCase
    {
        juce::String label;
        std::function<bool (const SessionStatus&)> reached;
        bool forceCpu = false;
        bool mustCancel = false; // slow enough to catch every time
    };

    void cancelAtEachStage()
    {
        beginTest ("Cancel at each stage leaves no playable partial cache");
        const auto file = clip ("cancel", 5.0);
        const auto key = computeFingerprint (file).fingerprint.cacheKey();

        const auto inStage = [] (worker::Stage stage)
        {
            return [stage] (const SessionStatus& status)
            { return status.state == State::preparing && status.stage.startsWith (stageDisplayName (stage)); };
        };

        const std::vector<StageCase> cases {
            { "checking the file", [] (const SessionStatus& s) { return s.state == State::checking; }, false, false },
            { "confirming artwork",
              [] (const SessionStatus& s) { return s.state == State::awaitingArtwork; },
              false,
              true },
            { "decoding", inStage (worker::Stage::decoding), false, false },
            { "separating (CPU)",
              [&] (const SessionStatus& s) { return inStage (worker::Stage::separating) (s) && s.progress > 0.1; },
              true,
              true },
            { "normalizing", inStage (worker::Stage::normalizing), false, false },
            { "analyzing tempo", inStage (worker::Stage::analyzingBpm), false, false },
            { "validating", inStage (worker::Stage::validating), false, false },
            { "finalizing", inStage (worker::Stage::finalizing), false, false },
        };

        for (const auto& stageCase : cases)
        {
            Rig rig (*this, stageCase.forceCpu);
            const auto before = rig.cache.listEntries().size();
            bool requested = false;
            juce::String requestedAt, reportedAt;

            // Cancel as soon as the stage is reported: the request is queued and runs on the next pump.
            rig.onStatus = [&] (const SessionStatus& status)
            {
                if (! requested && stageCase.reached (status))
                {
                    requested = true;
                    reportedAt = status.stage.isNotEmpty() ? status.stage : status.headline;
                    rig.queue.dispatcher() (
                        [&]
                        {
                            requestedAt = rig.session->status().stage.isNotEmpty() ? rig.session->status().stage
                                                                                   : rig.session->status().headline;
                            rig.session->cancel();
                        });
                }
            };

            rig.session->importSong (file);

            // Past the artwork question the job runs by itself.
            bool answered = false;
            const auto settled = rig.queue.pumpUntil (
                [&]
                {
                    if (! answered && rig.session->status().state == State::awaitingArtwork)
                    {
                        answered = true;

                        if (! requested)
                            rig.session->provideArtwork (makeFallbackChoice ("cancel"));
                    }

                    return ! rig.session->status().isBusy();
                },
                900000);
            expect (settled, stageCase.label + ": the session settles");

            const auto cancelled = rig.session->status().headline.containsIgnoreCase ("cancelled");
            logMessage (stageCase.label + ": "
                        + (requested ? "reported at \"" + reportedAt + "\", cancel sent at \"" + requestedAt + "\""
                                     : juce::String ("stage never reported"))
                        + "; result: "
                        + (cancelled                 ? "cancelled"
                           : rig.session->openSong() ? "finished first"
                                                     : rig.session->status().headline));

            if (stageCase.mustCancel)
                expect (cancelled, stageCase.label + " must end cancelled: " + rig.session->status().headline);

            for (const auto& entry : rig.cache.listEntries())
                expect (entry.isPlayable(),
                        stageCase.label + ": no partial entry " + entry.key + " (" + entry.reason + ")");

            expect (rig.cache.listJobs().empty(), stageCase.label + ": the job folder is deleted");

            if (cancelled)
            {
                expect (! rig.cache.findEntry (key).has_value(), stageCase.label + ": no cache entry");
                expectEquals (static_cast<int> (rig.cache.listEntries().size()), static_cast<int> (before));
                expect (! rig.session->openSong().has_value());
            }
            else if (rig.session->openSong())
            {
                // The job beat the cancel: that is a complete song, which the next case would reuse; drop it.
                expect (rig.cache.deleteEntry (key).wasOk());
            }
        }
    }
};

Phase6JourneyTests phase6JourneyTests;
} // namespace
} // namespace snds
