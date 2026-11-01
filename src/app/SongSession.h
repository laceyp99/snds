#pragma once

#include <juce_core/juce_core.h>

#include "app/AsyncTask.h"
#include "app/CacheStore.h"
#include "app/DataPaths.h"
#include "app/OriginalLocations.h"
#include "app/WorkerLocation.h"
#include "app/WorkerProcess.h"
#include "model/Artwork.h"
#include "model/SongImport.h"
#include "model/SourceFingerprint.h"

#include <functional>
#include <memory>
#include <optional>

namespace snds
{
/** What the session is doing right now. */
enum class SessionState
{
    idle,            // nothing in progress; a song may be open
    checking,        // validating the file and reading it for its fingerprint
    awaitingArtwork, // waiting for the user's artwork decision (provideArtwork)
    preparing,       // the worker is separating the song
    cancelling,      // cancel sent; waiting for the worker to stop
    locating,        // checking a file offered as the original of the open song
    failed           // the last operation failed; the message says why
};

/** A song opened from the cache. Its stems are always usable; the original only when found. */
struct OpenSong
{
    CacheEntry entry; // complete and playable
    juce::File originalFile;
    bool originalAvailable = false;
    juce::String originalNote; // for the UI when the original is not available

    /** True only in the onSongOpened call made when the user located the original of the song that
        is already open. It is the same song continuing (keep the playhead and the mix), unlike a
        song opened, or opened again, from the library. */
    bool originalJustLocated = false;

    const SongMetadata& song() const { return *entry.song; }

    /** True when tempo detection found nothing: the UI flags the 120 BPM default. */
    bool bpmIsDefault() const { return ! song().hasKnownBpm(); }

    juce::File artworkFile() const
    {
        return song().artwork.file.isEmpty() ? juce::File() : entry.directory.getChildFile (song().artwork.file);
    }
};

/** A snapshot of the current operation for the UI. */
struct SessionStatus
{
    SessionState state = SessionState::idle;
    juce::String songName; // file name of the song being imported, else empty
    juce::String headline; // one line
    juce::String detail;   // what happened or what to do next; may be empty
    juce::String guidance; // a concrete next step for failures; may be empty

    double progress = -1.0; // 0..1 overall, or -1 when unknown
    juce::String stage;     // for example "Separating stems (2 of 6)"
    std::optional<double> etaSeconds;
    juce::String device; // "cuda" or "cpu" once the worker says so

    bool canCancel = false;
    bool canRetry = false;      // prepare again with the same inputs
    bool canRetryOnCpu = false; // the worker suggested retry_cpu

    /** True in the status published when a song was just prepared by the worker (as opposed to
        opened from the library): the UI shows a ready notification. */
    bool justPrepared = false;
    double preparedSeconds = 0.0; // how long the preparation took

    std::optional<ImageData> detectedArtwork; // while awaiting the artwork decision
    juce::String failureCode;
    juce::String failureDetails; // diagnostic text for "Show details"

    bool isBusy() const noexcept { return state != SessionState::idle && state != SessionState::failed; }
};

struct SessionEnvironment
{
    DataPaths paths;
    WorkerLocation worker;
    CacheStore* cache = nullptr;
    OriginalLocations* originals = nullptr;

    std::function<bool()> forceCpu;                           // Settings.forceCpu
    std::function<juce::StringPairArray()> workerEnvironment; // extra variables for the worker process

    /** Called, on the dispatcher's thread, just before the session removes a cache entry, so
        playback can close the files it has open (Windows will not delete an open file). */
    std::function<void (const juce::String& key)> beforeCacheRemoval;

    Dispatcher dispatcher; // empty: the message thread
    int cancelGraceMs = 5000;
    int prepareTimeoutMs = 60 * 60 * 1000;
};

/** The primary import flow, driven from the message thread:

      importSong -> validate -> fingerprint (progress) -> cache lookup
        -> valid entry: open it (no artwork question, no worker)
        -> else: ask for the artwork decision -> provideArtwork -> `prepare` in the worker
           (stages, progress, Cancel) -> open the published package

    A non-playable entry in the way (incompatible, corrupt) is removed through the CacheStore
    before preparing; a cache_conflict from the worker removes it and offers to try again.
    Opening a package never needs the original: when it is missing the song opens with
    originalAvailable false, and locateOriginal accepts only a file with the same fingerprint.

    All callbacks arrive on the dispatcher's thread. Only one operation runs at a time; calls
    made while busy are ignored.
*/
class SongSession
{
public:
    explicit SongSession (SessionEnvironment environment);
    ~SongSession();

    std::function<void (const SessionStatus&)> onStatusChanged;

    /** Fires when a song is opened and again when its original becomes available. */
    std::function<void (const OpenSong&)> onSongOpened;

    /** Fires after closeSong: no song is open any more. */
    std::function<void()> onSongClosed;

    /** Fires after setBpmOverride changed the open song (it is not a new song: nothing reloads). */
    std::function<void (const OpenSong&)> onBpmChanged;

    const SessionStatus& status() const noexcept { return currentStatus; }
    const std::optional<OpenSong>& openSong() const noexcept { return opened; }

    void importSong (const juce::File& source);
    void provideArtwork (ArtworkChoice choice);
    void cancel();

    /** After a failure with canRetry: prepare again with the same file and artwork. */
    void retry();

    /** After a failure with canRetryOnCpu: prepare again, forcing the CPU. */
    void retryOnCpu();

    /** Opens a complete cache entry by key without a source file. */
    void openCached (const juce::String& key);

    /** For the open song when its original is missing: accepts the file only if its size and
        SHA-256 are those of the original. */
    void locateOriginal (const juce::File& candidate);

    /** Stores the user's tempo for the open song in its package (bpm.userOverride; nullopt goes
        back to the detected tempo) and updates the open song. Fires onBpmChanged. Fails, leaving
        everything as it was, if there is no open song or the file cannot be rewritten. */
    juce::Result setBpmOverride (const std::optional<SongMetadata::BpmOverride>& override);

    /** Lets go of the open song (for example before its cache entry is deleted). Does nothing
        while an operation is running or when no song is open. Fires onSongClosed. */
    void closeSong();

    /** Clears a failure message. */
    void dismissFailure();

private:
    struct Validated;
    struct Context;

    void publish (SessionStatus status);
    void fail (const juce::String& headline,
               const juce::String& detail,
               const juce::String& guidance = {},
               const juce::String& code = {},
               const juce::String& details = {});
    void backToIdle (const juce::String& headline, const juce::String& detail = {});

    void validationFinished (Validated validated);
    void fingerprintFinished (const FingerprintResult& result);
    void startPrepare (bool forceCpuOnce);
    void handleEvent (const worker::Event& event);
    void prepareFinished (const WorkerJobResult& result);
    void handleFailureEvent (const worker::FailureEvent& failure);
    void finishWithEntry (const juce::String& key, const juce::String& note);
    void openEntry (CacheEntry entry,
                    const std::optional<juce::File>& known,
                    std::optional<std::pair<double, juce::String>> preparedNow = std::nullopt);
    void locateFinished (const FingerprintResult& result, const juce::File& candidate);
    void releaseJob (bool deleteFolder);
    juce::Result removeNonPlayableEntry (const juce::String& key);

    SessionEnvironment env;
    SessionStatus currentStatus;
    std::optional<OpenSong> opened;

    std::unique_ptr<Context> context;
    std::unique_ptr<AsyncTask<Validated>> validation;
    std::unique_ptr<FingerprintJob> fingerprint;
    std::unique_ptr<FingerprintJob> locating;
    std::unique_ptr<WorkerProcess> process;
    std::optional<CacheStore::ActiveJob> activeJob;
    juce::String activeJobId;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SongSession)
};

/** "Reading the audio", "Separating stems", ... for a worker stage. */
juce::String stageDisplayName (worker::Stage stage);

/** Share of the whole job a stage represents, for the overall progress bar: separation is most
    of the time. */
double stageWeight (worker::Stage stage);

/** Overall progress 0..1 from the stage and the fraction within it. */
double overallProgress (worker::Stage stage, double fractionInStage);
} // namespace snds
