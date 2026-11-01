#include "app/PlaybackController.h"

#include "app/Logging.h"
#include "audio/FrameSource.h"
#include "model/SourceFingerprint.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace snds
{
struct PlaybackController::Prepared
{
    juce::String key;
    std::int64_t frames = 0;
    int sampleRate = 0;
    std::int64_t keepFrame = 0;
    std::array<std::unique_ptr<audio::FrameSource>, 4> stems;
    std::unique_ptr<audio::FrameSource> original;
    OriginalAvailability availability;
    juce::String error; // set when the stems cannot be played
};

namespace
{
/** Opens one stem and checks it against song.json. Returns null with `error` set on a problem. */
std::unique_ptr<audio::FrameSource> openStem (const OpenSong& song,
                                              int index,
                                              juce::AudioFormatManager& formats,
                                              juce::String& error)
{
    const auto& metadata = song.song();
    const auto& stem = metadata.stems[static_cast<size_t> (index)];
    const auto file = song.entry.directory.getChildFile (stem.file);
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr)
    {
        error = stem.file + " cannot be read.";
        return nullptr;
    }

    if (static_cast<int> (reader->sampleRate) != metadata.source.sampleRate || reader->numChannels != 2)
    {
        error = stem.file + " is " + juce::String (reader->sampleRate) + " Hz with "
              + juce::String (reader->numChannels) + " channel(s); the song needs "
              + juce::String (metadata.source.sampleRate) + " Hz stereo.";
        return nullptr;
    }

    if (reader->lengthInSamples != metadata.source.frames)
    {
        error = stem.file + " has " + juce::String (reader->lengthInSamples) + " frames; the song has "
              + juce::String (metadata.source.frames) + ".";
        return nullptr;
    }

    return std::make_unique<audio::ReaderFrameSource> (std::move (reader));
}

std::shared_ptr<PlaybackController::Prepared> prepare (const OpenSong& song,
                                                       std::int64_t keepFrame,
                                                       const std::shared_ptr<std::atomic<bool>>& cancelled)
{
    auto prepared = std::make_shared<PlaybackController::Prepared>();
    const auto& metadata = song.song();
    prepared->key = song.entry.key;
    prepared->frames = metadata.source.frames;
    prepared->sampleRate = metadata.source.sampleRate;
    prepared->keepFrame = keepFrame;

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::array<std::unique_ptr<audio::FrameSource>, 4> planning;

    for (int i = 0; i < 4; ++i)
    {
        prepared->stems[static_cast<size_t> (i)] = openStem (song, i, formats, prepared->error);

        if (prepared->stems[static_cast<size_t> (i)] == nullptr)
            return prepared;
    }

    if (! song.originalAvailable)
    {
        prepared->availability = { OriginalAvailability::Kind::missing, song.originalNote };
        return prepared;
    }

    const auto identity = computeFingerprint (song.originalFile, {}, [cancelled] { return cancelled->load(); });
    if (identity.status == FingerprintResult::Status::cancelled || cancelled->load())
        return prepared; // AsyncTask suppresses delivery when preparation is cancelled.

    const SourceFingerprint expected { metadata.source.sha256, metadata.source.sizeBytes };
    if (! identity.ok() || ! (identity.fingerprint == expected))
    {
        const auto reason =
            identity.ok() ? juce::String ("The original does not match the cached source content.") : identity.error;
        prepared->availability = { OriginalAvailability::Kind::unusable, reason };
        logWarning ("Original A/B disabled for " + metadata.source.fileName + ": " + reason);
        return prepared;
    }

    // The alignment check reads its own copies of the stems, off the streams the engine will use.
    for (int i = 0; i < 4; ++i)
    {
        juce::String ignored;
        planning[static_cast<size_t> (i)] = openStem (song, i, formats, ignored);

        if (planning[static_cast<size_t> (i)] == nullptr)
        {
            prepared->error = ignored;
            return prepared;
        }
    }

    auto plan = audio::planOriginal (song.originalFile,
                                     metadata.source.container,
                                     formats,
                                     metadata.source.frames,
                                     metadata.source.sampleRate,
                                     { planning[0].get(), planning[1].get(), planning[2].get(), planning[3].get() },
                                     [cancelled] { return cancelled->load(); });

    // Logged here, on the background thread: logging is thread-safe and the message is complete.
    logInfo ("Original A/B for " + metadata.source.fileName + ": " + plan.summary);

    if (plan.usable)
    {
        prepared->original = std::move (plan.source);
        prepared->availability = { OriginalAvailability::Kind::available, {} };
    }
    else
    {
        logWarning ("Original A/B disabled for " + metadata.source.fileName + ": " + plan.reason);
        prepared->availability = { OriginalAvailability::Kind::unusable, plan.reason };
    }

    return prepared;
}
} // namespace

//==============================================================================
PlaybackController::PlaybackController (audio::PlaybackEngine& engineToUse,
                                        audio::AudioDeviceController* deviceController,
                                        Dispatcher dispatcherToUse,
                                        MixService* mix)
    : engine (engineToUse),
      devices (deviceController),
      dispatcher (std::move (dispatcherToUse)),
      mixService (mix)
{
}

PlaybackController::~PlaybackController()
{
    alive->store (false);
    cancelPreparing();
    engine.unload();
}

void PlaybackController::announce()
{
    if (onChanged != nullptr)
        onChanged();
}

void PlaybackController::cancelPreparing()
{
    if (cancelFlag != nullptr)
        cancelFlag->store (true);

    task.reset(); // waits for the background work; it checks the flag and returns quickly
    cancelFlag.reset();
}

void PlaybackController::setSong (const std::optional<OpenSong>& song)
{
    cancelPreparing();
    waiting.reset();

    if (! song.has_value())
    {
        engine.unload();
        if (mixService != nullptr)
            mixService->endSong();
        engineHasSong = false;
        startedSinceLoad = false;
        songKey = {};
        failure = {};
        originalAvailability = {};
        loadState = PlaybackState::unloaded;
        announce();
        return;
    }

    // Locating the original of the open song continues it. Anything else, including opening the
    // same song again from the library, starts it over from the default mix.
    const bool sameSong = song->originalJustLocated && engineHasSong && songKey == song->entry.key;
    const auto keepFrame = sameSong ? engine.getPositionFrames() : 0;

    if (! sameSong)
    {
        engine.unload(); // a different song: stop and release the old files at once
        engineHasSong = false;
        startedSinceLoad = false;

        // Every newly opened song starts from the default mix, at its own tempo.
        if (mixService != nullptr)
            mixService->beginSong (song->song().effectiveBpm());
    }

    songKey = song->entry.key;
    startPreparing (*song, keepFrame);
}

void PlaybackController::startPreparing (const OpenSong& song, std::int64_t keepFrame)
{
    loadState = PlaybackState::preparing;
    failure = {};
    originalAvailability = { song.originalAvailable ? OriginalAvailability::Kind::checking
                                                    : OriginalAvailability::Kind::missing,
                             song.originalNote };
    announce();

    cancelFlag = std::make_shared<std::atomic<bool>> (false);
    task = std::make_unique<AsyncTask<std::shared_ptr<Prepared>>> (
        dispatcher,
        [song, keepFrame, flag = cancelFlag] { return prepare (song, keepFrame, flag); },
        [this] (std::shared_ptr<Prepared> prepared) { preparingFinished (std::move (prepared)); });
}

void PlaybackController::preparingFinished (std::shared_ptr<Prepared> prepared)
{
    task.reset();
    cancelFlag.reset();

    if (prepared->key != songKey)
        return;

    if (prepared->error.isNotEmpty())
    {
        logWarning ("Cannot play " + prepared->key + ": " + prepared->error);
        engine.unload();
        if (mixService != nullptr)
            mixService->endSong();
        engineHasSong = false;
        failure = prepared->error;
        originalAvailability = {};
        loadState = PlaybackState::failed;
        announce();
        return;
    }

    originalAvailability = prepared->availability;
    waiting = std::move (prepared);
    loadWhenPossible();
}

void PlaybackController::loadWhenPossible()
{
    if (waiting == nullptr)
        return;

    if (engine.getDeviceSampleRate() <= 0.0)
    {
        announce(); // prepared, waiting for an audio device
        return;
    }

    audio::PlaybackSources sources;
    sources.sampleRate = waiting->sampleRate;
    sources.lengthFrames = waiting->frames;
    sources.stems = std::move (waiting->stems);
    sources.original = std::move (waiting->original);
    const auto keepFrame = waiting->keepFrame;
    waiting.reset();

    if (! engine.load (std::move (sources), keepFrame))
    {
        failure = "The audio engine could not load this song.";
        logWarning (failure);
        engineHasSong = false;
        loadState = PlaybackState::failed;
        originalAvailability = {};
        announce();
        return;
    }

    engineHasSong = true;
    loadState = PlaybackState::ready;
    startedSinceLoad = false;
    announce();
}

void PlaybackController::releaseSong (const juce::String& key)
{
    if (songKey != key)
        return;

    cancelPreparing();
    waiting.reset();
    engine.unload();
    if (mixService != nullptr)
        mixService->endSong();
    engineHasSong = false;
    startedSinceLoad = false;
    songKey = {};
    failure = {};
    originalAvailability = {};
    loadState = PlaybackState::unloaded;
    announce();
}

void PlaybackController::deviceStatusChanged()
{
    if (waiting != nullptr)
        loadWhenPossible();

    announce();
}

//==============================================================================
void PlaybackController::play()
{
    if (! view().canPlay())
        return;

    engine.play();
    startedSinceLoad = true;
    announce();
}

void PlaybackController::pause()
{
    engine.pause();
    announce();
}

void PlaybackController::togglePlay()
{
    if (engine.isPlaying())
        pause();
    else
        play();
}

void PlaybackController::stop()
{
    if (! engineHasSong)
        return;

    engine.stop();
    startedSinceLoad = false;
    announce();
}

void PlaybackController::seekToFraction (double fraction)
{
    if (engineHasSong)
        engine.seek (static_cast<std::int64_t> (juce::jlimit (0.0, 1.0, fraction)
                                                * static_cast<double> (engine.getLengthFrames())));
}

void PlaybackController::setLooping (bool shouldLoop)
{
    engine.setLooping (shouldLoop);
    announce();
}

void PlaybackController::setOriginalSelected (bool shouldPlayOriginal)
{
    if (engineHasSong && originalAvailability.isAvailable())
        engine.setOriginalSelected (shouldPlayOriginal);

    announce();
}

PlaybackView PlaybackController::view() const
{
    PlaybackView v;
    v.original = originalAvailability;
    v.looping = engine.isLooping();

    if (devices != nullptr)
        v.device = devices->getStatus();
    else
        v.device.state = engine.getDeviceSampleRate() > 0.0 ? audio::DeviceStatus::State::running
                                                            : audio::DeviceStatus::State::notStarted;

    if (loadState == PlaybackState::preparing)
    {
        v.state = PlaybackState::preparing;
        v.message = waiting != nullptr ? "Waiting for an audio device." : juce::String();
        return v;
    }

    if (loadState == PlaybackState::failed)
    {
        v.state = PlaybackState::failed;
        v.message = failure;
        return v;
    }

    if (! engineHasSong)
        return v;

    v.originalSelected = engine.isOriginalSelected();
    v.buffering = engine.isBuffering();
    v.underruns = engine.getUnderrunCount();
    v.durationSeconds = engine.getDurationSeconds();
    v.positionSeconds = engine.getPositionSeconds();
    v.fraction = v.durationSeconds > 0.0 ? juce::jlimit (0.0, 1.0, v.positionSeconds / v.durationSeconds) : 0.0;

    if (engine.isPlaying() || engine.isAudible())
        v.state = PlaybackState::playing;
    else
        v.state = (startedSinceLoad || engine.getPositionFrames() > 0) ? PlaybackState::paused : PlaybackState::ready;

    return v;
}
} // namespace snds
