#include "app/SongSession.h"

#include "app/ErrorReport.h"
#include "app/Logging.h"
#include "app/ProgressText.h"
#include "app/SongPackageEdit.h"

#include <cmath>

namespace snds
{
using namespace worker;

namespace
{
juce::String makeJobId()
{
    return juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + "-"
         + juce::String::toHexString (juce::Random::getSystemRandom().nextInt (0xFFFFFF)).paddedLeft ('0', 6);
}

juce::String stageLine (Stage stage, int index)
{
    return stageDisplayName (stage) + " (" + juce::String (index) + " of " + juce::String (stageCount) + ")";
}

juce::String titleOf (const SongMetadata& song)
{
    return song.source.fileName;
}

struct FailureText
{
    juce::String headline;
    juce::String guidance;
};

FailureText describeFailureCode (const FailureEvent& failure)
{
    // The next step for each code lives in the error catalogue (app/ErrorReport).
    return { failure.message.isNotEmpty() ? failure.message : juce::String ("The worker reported a failure."),
             guidanceForCode (failure.code, failure.suggestedAction) };
}
} // namespace

//==============================================================================
juce::Result SongSession::removeNonPlayableEntry (const juce::String& key)
{
    if (env.beforeCacheRemoval != nullptr)
        env.beforeCacheRemoval (key);

    return env.cache->removeNonPlayableEntry (key);
}

juce::String stageDisplayName (Stage stage)
{
    switch (stage)
    {
        case Stage::decoding:
            return "Reading the audio";
        case Stage::separating:
            return "Separating stems";
        case Stage::normalizing:
            return "Aligning stems";
        case Stage::analyzingBpm:
            return "Detecting the tempo";
        case Stage::validating:
            return "Checking the result";
        case Stage::finalizing:
            return "Saving the song";
    }

    return {};
}

double stageWeight (Stage stage)
{
    switch (stage)
    {
        case Stage::decoding:
            return 0.05;
        case Stage::separating:
            return 0.80;
        case Stage::normalizing:
            return 0.05;
        case Stage::analyzingBpm:
            return 0.05;
        case Stage::validating:
            return 0.03;
        case Stage::finalizing:
            return 0.02;
    }

    return 0.0;
}

double overallProgress (Stage stage, double fractionInStage)
{
    double before = 0.0;

    for (int index = 0; index < static_cast<int> (stage); ++index)
        before += stageWeight (static_cast<Stage> (index));

    return juce::jlimit (0.0, 1.0, before + stageWeight (stage) * juce::jlimit (0.0, 1.0, fractionInStage));
}

//==============================================================================
struct SongSession::Validated
{
    ImportResult import;
    std::optional<ImageData> artwork;
};

struct SongSession::Context
{
    juce::File source;
    juce::String songName;
    SourceInfo info;
    std::optional<ImageData> detected;
    SourceFingerprint fingerprint;
    std::optional<ArtworkChoice> artwork;
    juce::String key;

    juce::String jobId;
    juce::File jobDir;
    bool cancelRequested = false;
    Stage stage = Stage::decoding;
    int stageIndex = 1;
    juce::String device;
    bool retryable = false; // a failed attempt can be repeated with the same inputs
    double startedMs = 0.0; // when the worker was started
};

//==============================================================================
SongSession::SongSession (SessionEnvironment environment)
    : env (std::move (environment))
{
    jassert (env.cache != nullptr && env.originals != nullptr);

    if (! env.dispatcher)
        env.dispatcher = messageThreadDispatcher();
}

SongSession::~SongSession()
{
    // Stop everything that could call back before the members go away.
    process.reset();
    fingerprint.reset();
    locating.reset();
    validation.reset();
    activeJob.reset();
}

void SongSession::publish (SessionStatus status)
{
    currentStatus = std::move (status);

    if (onStatusChanged != nullptr)
        onStatusChanged (currentStatus);
}

void SongSession::fail (const juce::String& headline,
                        const juce::String& detail,
                        const juce::String& guidance,
                        const juce::String& code,
                        const juce::String& details)
{
    logWarning ("Session failure: " + headline + (detail.isNotEmpty() ? " " + detail : juce::String()));

    SessionStatus status;
    status.state = SessionState::failed;
    status.songName = context != nullptr ? context->songName : juce::String();
    status.headline = headline;
    status.detail = detail;
    status.guidance = guidance;
    status.failureCode = code;
    status.failureDetails = details;
    publish (std::move (status));
}

void SongSession::backToIdle (const juce::String& headline, const juce::String& detail)
{
    SessionStatus status;
    status.headline = headline;
    status.detail = detail;
    publish (std::move (status));
}

juce::Result SongSession::setBpmOverride (const std::optional<SongMetadata::BpmOverride>& override)
{
    if (! opened.has_value() || ! opened->entry.song.has_value())
        return juce::Result::fail ("No song is open.");

    if (const auto written = writeBpmOverride (opened->entry.directory, override); written.failed())
        return written;

    opened->entry.song->bpm.userOverride = override;

    if (onBpmChanged != nullptr)
        onBpmChanged (*opened);

    return juce::Result::ok();
}

void SongSession::closeSong()
{
    if (currentStatus.isBusy() || ! opened.has_value())
        return;

    opened.reset();
    context.reset();
    backToIdle ({});

    if (onSongClosed)
        onSongClosed();
}

void SongSession::dismissFailure()
{
    if (currentStatus.state == SessionState::failed)
        backToIdle ({});
}

//==============================================================================
void SongSession::importSong (const juce::File& source)
{
    if (currentStatus.isBusy())
        return;

    context = std::make_unique<Context>();
    context->source = source;
    context->songName = source.getFileName();

    SessionStatus status;
    status.state = SessionState::checking;
    status.songName = context->songName;
    status.headline = "Checking " + context->songName;
    status.canCancel = true;
    publish (std::move (status));

    validation = std::make_unique<AsyncTask<Validated>> (
        env.dispatcher,
        [source]
        {
            Validated result;
            result.import = validateSourceFile (source);

            if (result.import.accepted())
                result.artwork = extractEmbeddedArtwork (source);

            return result;
        },
        [this] (Validated validated) { validationFinished (std::move (validated)); });
}

void SongSession::validationFinished (Validated validated)
{
    validation.reset();

    if (context->cancelRequested)
        return backToIdle ("Import cancelled.");

    if (! validated.import.accepted())
        return fail ("This file can't be imported.", validated.import.message, {}, "import_rejected");

    context->info = validated.import.info;
    context->detected = std::move (validated.artwork);

    fingerprint = std::make_unique<FingerprintJob> (
        context->source,
        [this] (juce::int64 done, juce::int64 total)
        {
            auto status = currentStatus;
            status.stage = "Reading the file";
            status.progress = total > 0 ? static_cast<double> (done) / static_cast<double> (total) : -1.0;
            publish (std::move (status));
        },
        [this] (const FingerprintResult& result) { fingerprintFinished (result); },
        env.dispatcher);

    auto status = currentStatus;
    status.headline = "Reading " + context->songName;
    status.stage = "Reading the file";
    publish (std::move (status));
}

void SongSession::fingerprintFinished (const FingerprintResult& result)
{
    fingerprint.reset();

    if (result.status == FingerprintResult::Status::cancelled || context->cancelRequested)
        return backToIdle ("Import cancelled.");

    if (! result.ok())
        return fail ("This file can't be read.", result.error, {}, "fingerprint_failed");

    context->fingerprint = result.fingerprint;
    context->key = result.fingerprint.cacheKey();

    if (const auto existing = env.cache->findEntry (context->key))
    {
        if (existing->isPlayable())
        {
            logInfo ("Opening " + context->key + " from the cache; nothing to prepare.");
            openEntry (*existing, context->source);
            return;
        }

        logInfo ("Replacing the " + SongLoadResult::statusName (existing->status) + " cache entry " + context->key
                 + ": " + existing->reason);

        if (const auto removed = removeNonPlayableEntry (context->key); removed.failed())
            return fail ("An unusable earlier copy of this song is in the way.",
                         removed.getErrorMessage(),
                         "Close any program using SNDS files, then try again.",
                         "cache_remove_failed");
    }

    SessionStatus status;
    status.state = SessionState::awaitingArtwork;
    status.songName = context->songName;
    status.headline = "Confirm the artwork for " + context->songName;
    status.detectedArtwork = context->detected;
    status.canCancel = true;
    publish (std::move (status));
}

void SongSession::provideArtwork (ArtworkChoice choice)
{
    if (currentStatus.state != SessionState::awaitingArtwork || context == nullptr)
        return;

    context->artwork = std::move (choice);
    startPrepare (false);
}

//==============================================================================
void SongSession::startPrepare (bool forceCpuOnce)
{
    auto& ctx = *context;
    ctx.cancelRequested = false;
    ctx.retryable = true;
    ctx.device = {};

    // The mark comes before the folder exists, so a cleanup pass can never see it unmarked.
    const auto previousJob = ctx.jobId; // a failed attempt kept for diagnosis; this one replaces it
    ctx.jobId = makeJobId();
    activeJob.reset();
    activeJob = env.cache->markJobActive (ctx.jobId);

    if (previousJob.isNotEmpty())
        env.cache->deleteJobFolder (previousJob);
    activeJobId = ctx.jobId;
    ctx.jobDir = env.paths.jobsDirectory().getChildFile (ctx.jobId);

    const auto artworkFile = writeArtworkToJobDir (*ctx.artwork, ctx.jobDir);

    if (artworkFile == juce::File() || ! env.paths.cacheDirectory().createDirectory().wasOk())
    {
        releaseJob (false);
        return fail ("SNDS could not prepare its work folder.",
                     "Check that " + env.paths.home.getFullPathName() + " is writable and has free space.",
                     {},
                     "work_folder",
                     ctx.jobDir.getFullPathName());
    }

    PrepareRequest request;
    request.jobId = ctx.jobId;
    request.sourceFile = ctx.source;
    request.sourceSizeBytes = ctx.fingerprint.sizeBytes;
    request.sourceSha256 = ctx.fingerprint.sha256;
    request.jobDir = ctx.jobDir;
    request.cacheDir = env.paths.cacheDirectory().getChildFile (ctx.key);
    request.forceCpu = forceCpuOnce || (env.forceCpu != nullptr && env.forceCpu());
    request.artwork = PrepareRequest::Artwork { artworkFile, ctx.artwork->originId() };

    auto options = WorkerProcess::makePrepareOptions (env.worker, request);
    options.cancelGraceMs = env.cancelGraceMs;
    options.timeoutMs = env.prepareTimeoutMs;

    if (env.workerEnvironment != nullptr)
        options.extraEnvironment = env.workerEnvironment();

    WorkerProcess::Callbacks callbacks;
    callbacks.onEvent = [this] (const Event& event) { handleEvent (event); };
    callbacks.onFinished = [this] (const WorkerJobResult& result) { prepareFinished (result); };

    SessionStatus status;
    status.state = SessionState::preparing;
    status.songName = ctx.songName;
    status.headline = "Preparing " + ctx.songName;
    status.stage = "Starting the worker";
    status.progress = 0.0;
    status.canCancel = true;
    publish (std::move (status));

    logInfo ("Preparing " + ctx.songName + " as " + ctx.key + " (job " + ctx.jobId + (request.forceCpu ? ", CPU" : "")
             + ")");

    ctx.startedMs = juce::Time::getMillisecondCounterHiRes();
    process = std::make_unique<WorkerProcess> (std::move (options), std::move (callbacks), env.dispatcher);
    process->start();
}

void SongSession::handleEvent (const Event& event)
{
    if (currentStatus.state != SessionState::preparing && currentStatus.state != SessionState::cancelling)
        return;

    auto& ctx = *context;
    auto status = currentStatus;

    if (const auto* accepted = std::get_if<AcceptedEvent> (&event))
    {
        ctx.device = accepted->device;
        status.device = accepted->device;
    }
    else if (const auto* stage = std::get_if<StageEvent> (&event))
    {
        ctx.stage = stage->stage;
        ctx.stageIndex = stage->index;
        status.stage = stageLine (stage->stage, stage->index);
        status.progress = overallProgress (stage->stage, 0.0);
        status.etaSeconds.reset();
    }
    else if (const auto* progress = std::get_if<ProgressEvent> (&event))
    {
        status.stage = stageLine (progress->stage, ctx.stageIndex);
        status.progress = overallProgress (progress->stage, progress->fraction);
        status.etaSeconds = progress->etaSeconds;
    }
    else
    {
        return; // terminal events are handled with the exit code in prepareFinished
    }

    if (currentStatus.state == SessionState::cancelling)
    {
        status.state = SessionState::cancelling;
        status.canCancel = false;
    }

    publish (std::move (status));
}

void SongSession::cancel()
{
    switch (currentStatus.state)
    {
        case SessionState::checking:
            context->cancelRequested = true;

            if (fingerprint != nullptr)
                fingerprint->cancel();

            break;

        case SessionState::awaitingArtwork:
            backToIdle ("Import cancelled.");
            break;

        case SessionState::preparing:
        {
            context->cancelRequested = true;
            auto status = currentStatus;
            status.state = SessionState::cancelling;
            status.headline = "Cancelling " + context->songName;
            status.canCancel = false;
            status.etaSeconds.reset();
            publish (std::move (status));

            if (process != nullptr)
                process->cancel();

            break;
        }

        case SessionState::locating:
            if (locating != nullptr)
                locating->cancel();

            break;

        case SessionState::idle:
        case SessionState::cancelling:
        case SessionState::failed:
            break;
    }
}

void SongSession::retry()
{
    if (currentStatus.state == SessionState::failed && currentStatus.canRetry && context != nullptr)
        startPrepare (false);
}

void SongSession::retryOnCpu()
{
    if (currentStatus.state == SessionState::failed && currentStatus.canRetryOnCpu && context != nullptr)
        startPrepare (true);
}

void SongSession::releaseJob (bool deleteFolder)
{
    const auto jobId = activeJobId;
    activeJob.reset(); // the worker is gone: the folder is no longer owned

    if (deleteFolder && jobId.isNotEmpty())
        if (const auto deleted = env.cache->deleteJobFolder (jobId); deleted.failed())
            logWarning (deleted.getErrorMessage());

    activeJobId = {};
}

//==============================================================================
void SongSession::prepareFinished (const WorkerJobResult& result)
{
    process.reset();
    auto& ctx = *context;
    const auto key = ctx.key;

    if (! result.launched)
    {
        releaseJob (false);
        return fail ("The worker could not be started.",
                     result.launchError,
                     "Run scripts\\bootstrap.ps1, then press Re-check.",
                     "launch_failed",
                     result.launchError);
    }

    switch (result.outcome)
    {
        case JobOutcome::succeeded:
        {
            const auto entry = env.cache->findEntry (key);

            if (! entry || ! entry->isPlayable())
            {
                releaseJob (false);
                return fail ("The worker finished, but the song it published is not usable.",
                             entry ? entry->reason : juce::String ("The cache entry is missing."),
                             {},
                             "published_invalid",
                             result.stderrTail);
            }

            releaseJob (true);
            openEntry (
                *entry,
                ctx.source,
                std::make_pair ((juce::Time::getMillisecondCounterHiRes() - ctx.startedMs) / 1000.0, ctx.device));
            return;
        }

        case JobOutcome::cancelled:
        case JobOutcome::killed:
        {
            // A cancel that arrived after the package was published still leaves a valid song.
            if (const auto entry = env.cache->findEntry (key))
            {
                if (entry->isPlayable())
                {
                    releaseJob (true);
                    openEntry (*entry, ctx.source);
                    return;
                }

                removeNonPlayableEntry (key);
            }

            releaseJob (true);
            ctx.retryable = false;

            if (result.timedOut)
                return fail ("Preparing " + ctx.songName + " took too long and was stopped.",
                             "Nothing was saved.",
                             "Try again, or retry on the CPU if the GPU may be stalling.",
                             "timeout",
                             result.stderrTail);

            return backToIdle ("Cancelled. Nothing was saved.", ctx.songName);
        }

        case JobOutcome::failed:
        {
            const auto* failure = result.terminalEvent ? std::get_if<FailureEvent> (&*result.terminalEvent) : nullptr;

            if (failure != nullptr)
                return handleFailureEvent (*failure);

            break;
        }

        case JobOutcome::crashed:
        case JobOutcome::protocolViolation:
            break;
    }

    // The worker crashed, spoke an unsupported protocol, or ended inconsistently. The job
    // folder stays (with worker-stderr.log) for diagnosis; quarantine removes any audio later.
    releaseJob (false);

    if (const auto entry = env.cache->findEntry (key); entry && entry->isPlayable())
        return openEntry (*entry, ctx.source);

    removeNonPlayableEntry (key);

    const auto what = result.protocolError.isNotEmpty()
                        ? result.protocolError
                        : "The worker stopped unexpectedly (" + describe (result.outcome) + ", exit code "
                              + juce::String (result.exitCode) + ").";
    fail ("Preparing " + ctx.songName + " failed.",
          what,
          "Run scripts\\diagnostics.ps1 for a report. The details are in the job folder: "
              + ctx.jobDir.getFullPathName(),
          result.protocolError.isNotEmpty() ? "unsupported_protocol" : "worker_crashed",
          result.stderrTail);

    auto status = currentStatus;
    status.canRetry = true;
    publish (std::move (status));
}

void SongSession::handleFailureEvent (const FailureEvent& failure)
{
    auto& ctx = *context;
    const auto key = ctx.key;
    auto text = describeFailureCode (failure);
    const bool conflict = failure.code == "cache_conflict";

    releaseJob (false);

    if (conflict)
    {
        const auto entry = env.cache->findEntry (key);

        if (entry && entry->isPlayable())
            return openEntry (*entry, ctx.source); // another copy became valid in the meantime

        if (const auto removed = removeNonPlayableEntry (key); removed.failed())
        {
            text.guidance = removed.getErrorMessage();
        }
        else
        {
            text.headline = "An earlier, unusable copy of " + ctx.songName + " was in the way and has been removed.";
            text.guidance = "Press Try again to prepare the song.";
        }
    }

    fail (text.headline, failure.message, text.guidance, failure.code, failure.details);

    auto status = currentStatus;
    // Repeating a CUDA failure on CUDA is pointless: that case offers the CPU instead.
    status.canRetry = conflict || (failure.retryable && failure.suggestedAction != "retry_cpu");
    status.canRetryOnCpu = failure.suggestedAction == "retry_cpu" && ctx.device != "cpu";
    publish (std::move (status));
}

//==============================================================================
void SongSession::openEntry (CacheEntry entry,
                             const std::optional<juce::File>& known,
                             std::optional<std::pair<double, juce::String>> preparedNow)
{
    OpenSong song;
    song.entry = std::move (entry);
    const auto& metadata = song.song();
    const auto key = song.entry.key;

    juce::StringArray candidates;

    if (known)
        candidates.add (known->getFullPathName());

    if (const auto remembered = env.originals->find (key))
        candidates.add (remembered->getFullPathName());

    candidates.add (metadata.source.path);
    candidates.removeDuplicates (false);

    juce::String note;

    for (const auto& path : candidates)
    {
        const juce::File file (path);

        if (! file.existsAsFile())
            continue;

        if (file.getSize() != metadata.source.sizeBytes)
        {
            note = file.getFullPathName() + " has a different size than the original; it has changed.";
            continue;
        }

        song.originalFile = file;
        song.originalAvailable = true;
        note = {};
        break;
    }

    if (! song.originalAvailable)
    {
        song.originalNote = note.isNotEmpty() ? note
                                              : "The original (" + metadata.source.fileName
                                                    + ") was not found. Stems still work; Original A/B is unavailable.";
        song.originalFile = juce::File (metadata.source.path);
    }
    else if (known && song.originalFile == *known)
    {
        env.originals->remember (key, song.originalFile);
    }

    opened = std::move (song);
    releaseJob (false);

    SessionStatus status;
    status.headline = "Ready: " + titleOf (opened->song());

    if (preparedNow)
    {
        status.justPrepared = true;
        status.preparedSeconds = preparedNow->first;
        status.detail = describePrepared (preparedNow->first, preparedNow->second) + " It is loaded in the player.";
    }
    else
    {
        status.detail = "Opened from your library; it was already prepared, so nothing was separated.";
    }

    if (! opened->originalAvailable)
        status.detail += " " + opened->originalNote;
    publish (std::move (status));

    if (onSongOpened != nullptr)
        onSongOpened (*opened);
}

void SongSession::openCached (const juce::String& key)
{
    if (currentStatus.isBusy())
        return;

    context.reset();
    const auto entry = env.cache->findEntry (key);

    if (! entry)
        return fail ("That song is no longer in the cache.", {}, "Import the original file again.", "cache_missing");

    if (! entry->isPlayable())
        return fail ("That cached song can't be used.",
                     entry->reason,
                     "Import the original file again to rebuild it.",
                     "cache_" + SongLoadResult::statusName (entry->status));

    openEntry (*entry, std::nullopt);
}

//==============================================================================
void SongSession::locateOriginal (const juce::File& candidate)
{
    if (currentStatus.isBusy() || ! opened.has_value() || opened->originalAvailable)
        return;

    const auto& metadata = opened->song();

    if (! candidate.existsAsFile() || candidate.getSize() != metadata.source.sizeBytes)
    {
        context.reset();
        return fail (
            "That is not the original of this song.",
            candidate.getFileName()
                + (candidate.existsAsFile() ? " has a different size than " : " was not found; the original is ")
                + metadata.source.fileName + ".",
            "Choose the exact file this song was made from.",
            "locate_mismatch");
    }

    context.reset();

    SessionStatus status;
    status.state = SessionState::locating;
    status.headline = "Checking " + candidate.getFileName();
    status.stage = "Comparing with the original";
    status.canCancel = true;
    publish (std::move (status));

    locating = std::make_unique<FingerprintJob> (
        candidate,
        [this] (juce::int64 done, juce::int64 total)
        {
            auto s = currentStatus;
            s.progress = total > 0 ? static_cast<double> (done) / static_cast<double> (total) : -1.0;
            publish (std::move (s));
        },
        [this, candidate] (const FingerprintResult& result) { locateFinished (result, candidate); },
        env.dispatcher);
}

void SongSession::locateFinished (const FingerprintResult& result, const juce::File& candidate)
{
    locating.reset();

    if (result.status == FingerprintResult::Status::cancelled)
        return backToIdle ("Locate cancelled.");

    if (! result.ok())
        return fail ("That file can't be read.", result.error, {}, "fingerprint_failed");

    if (! opened || result.fingerprint.cacheKey() != opened->entry.key)
        return fail ("That is not the original of this song.",
                     candidate.getFileName() + " has the same size but different content.",
                     "Choose the exact file this song was made from.",
                     "locate_mismatch");

    opened->originalFile = candidate;
    opened->originalAvailable = true;
    opened->originalNote = {};
    opened->originalJustLocated = true;

    if (! env.originals->remember (opened->entry.key, candidate))
        logWarning ("Could not save the original's location.");

    backToIdle ("Original located.", candidate.getFullPathName());

    if (onSongOpened != nullptr)
        onSongOpened (*opened);

    opened->originalJustLocated = false;
}
} // namespace snds
