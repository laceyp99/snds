#pragma once

#include "app/MixService.h"
#include "app/Settings.h"
#include "audio/AudioDeviceController.h"
#include "audio/PlaybackEngine.h"

#include <juce_audio_devices/juce_audio_devices.h>

namespace snds
{
/** The app's audio: the playback engine, the JUCE device manager, and the controller that keeps
    the two connected. Persists the user's device choice in Settings.

    Message thread only (except the engine's audio callback). Nothing opens a device until
    start(), so constructing it is free of side effects.
*/
class AudioService
{
public:
    /** `settings` may be null (no persistence). It must outlive this object. */
    explicit AudioService (Settings* settings);
    ~AudioService();

    /** Opens the saved device, or the system default. Returns an error text, empty if running. */
    juce::String start();

    audio::PlaybackEngine& engine() noexcept { return playback; }
    MixService& mix() noexcept { return mixService; }
    audio::AudioDeviceController& devices() noexcept { return controller; }

private:
    Settings* settings;
    MixService mixService; // outlives the engine: its sessions hold processors it created
    audio::PlaybackEngine playback;
    juce::AudioDeviceManager manager;
    audio::AudioDeviceController controller;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioService)
};
} // namespace snds
