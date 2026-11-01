#pragma once

#include "app/AsyncTask.h"
#include "app/MixService.h"
#include "app/SongSession.h"
#include "audio/AudioDeviceController.h"
#include "audio/OriginalAlignment.h"
#include "audio/PlaybackEngine.h"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

namespace snds
{
enum class PlaybackState
{
    unloaded,  // no song
    preparing, // opening the stems and checking the original (background)
    ready,     // loaded, at the start
    playing,
    paused,
    failed // the song's audio could not be loaded; see PlaybackView::message
};

/** Whether Original/Stem Mix A/B can be used for the open song, and if not, why. */
struct OriginalAvailability
{
    enum class Kind
    {
        none,      // no song
        checking,  // verifying the content identity and alignment in the background
        available, // matches the cached source and is aligned with the stems, A/B works
        missing,   // the original file is not found
        unusable   // found, but identity or alignment verification failed; `reason` says why
    };

    Kind kind = Kind::none;
    juce::String reason;

    bool isAvailable() const noexcept { return kind == Kind::available; }
};

/** Everything the transport bar shows, taken at one moment. */
struct PlaybackView
{
    PlaybackState state = PlaybackState::unloaded;
    juce::String message; // why a song failed to load
    OriginalAvailability original;
    audio::DeviceStatus device;

    bool looping = false;
    bool originalSelected = false;
    bool buffering = false; // waiting for read-ahead after a start, seek or underrun
    int underruns = 0;

    double positionSeconds = 0.0;
    double durationSeconds = 0.0;
    double fraction = 0.0; // position / duration, 0..1

    bool hasSong() const noexcept { return state != PlaybackState::unloaded && state != PlaybackState::failed; }
    bool deviceUsable() const noexcept { return device.isUsable(); }
    bool canPlay() const noexcept
    {
        return (state == PlaybackState::ready || state == PlaybackState::playing || state == PlaybackState::paused)
            && deviceUsable();
    }
    bool canSeek() const noexcept
    {
        return state == PlaybackState::ready || state == PlaybackState::playing || state == PlaybackState::paused;
    }
};

/** Loads the open song into the PlaybackEngine and exposes the transport to the UI.

    A song is opened off the message thread (stem files validated against song.json, the original
    hashed against the cached source and aligned with the stems by planOriginal), then handed to
    the engine, paused at the start. If no audio device is running yet the prepared song waits for one.

    Message thread only. The engine's own callback is the only other thread that touches playback.
*/
class PlaybackController
{
public:
    /** `devices` and `mix` may be null (no device layer or no mix, as in offline tests). */
    PlaybackController (audio::PlaybackEngine& engine,
                        audio::AudioDeviceController* devices,
                        Dispatcher dispatcher = messageThreadDispatcher(),
                        MixService* mix = nullptr);
    ~PlaybackController();

    /** Called when the state, the original's availability or the device status changes. Position
        ticks are not announced: poll view(). */
    std::function<void()> onChanged;

    /** Opens `song` (or unloads for nullopt). Opening the same song again, as happens when its
        original is located, keeps the playhead and leaves the transport paused. */
    void setSong (const std::optional<OpenSong>& song);

    /** Releases every file of `key` before it is deleted from the cache. */
    void releaseSong (const juce::String& key);

    /** To call when the audio device started, stopped or changed. */
    void deviceStatusChanged();

    void play();
    void pause();
    void togglePlay();
    void stop();
    void seekToFraction (double fraction);
    void setLooping (bool shouldLoop);
    void setOriginalSelected (bool shouldPlayOriginal);

    PlaybackView view() const;
    const juce::String& loadedKey() const noexcept { return songKey; }

    /** The result of preparing a song in the background (defined in the .cpp). */
    struct Prepared;

private:
    void announce();
    void startPreparing (const OpenSong& song, std::int64_t keepFrame);
    void preparingFinished (std::shared_ptr<Prepared> prepared);
    void loadWhenPossible();
    void cancelPreparing();

    audio::PlaybackEngine& engine;
    audio::AudioDeviceController* devices;
    MixService* mixService; // the interactive mix; reset for each newly opened song (may be null)
    Dispatcher dispatcher;

    PlaybackState loadState = PlaybackState::unloaded;
    juce::String failure;
    OriginalAvailability originalAvailability;
    juce::String songKey;
    bool engineHasSong = false;
    bool startedSinceLoad = false;
    std::int64_t resumeFrame = 0;
    std::shared_ptr<Prepared> waiting; // prepared, but no device to load it into yet

    std::unique_ptr<AsyncTask<std::shared_ptr<Prepared>>> task;
    std::shared_ptr<std::atomic<bool>> cancelFlag;
    std::shared_ptr<std::atomic<bool>> alive { std::make_shared<std::atomic<bool>> (true) };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlaybackController)
};
} // namespace snds
