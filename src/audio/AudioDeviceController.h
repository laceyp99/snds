#pragma once

#include "audio/PlaybackEngine.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace snds::audio
{
/** What the output device is doing, for the UI. */
struct DeviceStatus
{
    enum class State
    {
        notStarted, // initialise() has not run
        running,    // the engine is attached to the preferred device
        fallback,   // running on another device because the preferred one is missing
        lost,       // the preferred device is gone and no device could be opened
                    // (the transport is stopped, its position kept)
        failed      // no device could be opened, or the device reported an error
    };

    State state = State::notStarted;
    juce::String typeName;
    juce::String deviceName; // the device actually in use; empty when none
    juce::String preferredDeviceName;
    double sampleRate = 0.0;
    int bufferSize = 0;
    juce::String message; // one line for the user; empty when all is well

    bool isUsable() const noexcept { return state == State::running || state == State::fallback; }
};

/** Connects the PlaybackEngine to a JUCE AudioDeviceManager and keeps it connected.

    - The engine is the manager's audio callback; every other member is message-thread only.
    - The user's choice (the "preferred" device) is the state XML of the manager while the
      active device is the preferred one. It is reported through onPersist for the settings
      file; a fallback device JUCE picks while the preferred one is missing is never persisted.
    - Any device stop (switching device, losing it, an error) stops the transport at its
      position (PlaybackEngine::deviceStopped) and never resumes by itself. A new device rate
      or buffer size re-prepares the engine's resampler from aboutToStart.
    - When the preferred device disappears the status becomes `lost`. When it returns (the
      manager's device list changes, or reconnect() is called) it is reopened and the status
      goes back to `running`; the transport stays paused at the same position.

    The manager is injected, so tests drive the same logic through a simulated device type.
*/
class AudioDeviceController final : private juce::AudioIODeviceCallback, private juce::ChangeListener
{
public:
    AudioDeviceController (PlaybackEngine& engine, juce::AudioDeviceManager& manager);
    ~AudioDeviceController() override;

    /** Called on the message thread when the status changes. */
    std::function<void (const DeviceStatus&)> onStatusChanged;

    /** Called on the message thread with the XML to store when the preferred device or its
        settings change. */
    std::function<void (const juce::String& stateXml)> onPersist;

    /** Opens the saved device (`savedStateXml`, as given to onPersist; may be empty) or the
        system default if that fails. Returns an error text, empty when a device is running. */
    juce::String initialise (const juce::String& savedStateXml);

    /** Closes the device and detaches from the manager. */
    void shutdown();

    /** Tries to reopen the preferred device now (a "Reconnect" button). */
    void reconnect();

    /** While true (the settings dialog is open), whatever device the manager is on is taken
        as the user's choice. */
    void setUserEditing (bool isEditing);

    const DeviceStatus& getStatus() const noexcept { return status; }
    juce::AudioDeviceManager& getManager() noexcept { return manager; }

    /** Callbacks seen by the engine since start; for diagnostics and tests. */
    int getCallbackCount() const noexcept { return callbacks.load (std::memory_order_relaxed); }

private:
    // AudioIODeviceCallback: audio thread for the I/O callback, otherwise the thread that
    // opened or closed the device.
    void audioDeviceIOCallbackWithContext (const float* const* input,
                                           int numInputChannels,
                                           float* const* output,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceError (const juce::String& errorMessage) override;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    void evaluate();
    bool preferredIsAvailable() const;
    bool activeIsPreferred() const;
    void openPreferred();
    void publish (DeviceStatus next);
    void rememberPreferred();

    PlaybackEngine& engine;
    juce::AudioDeviceManager& manager;
    DeviceStatus status;

    juce::String preferredType;
    juce::String preferredOutput;
    juce::String preferredStateXml;
    juce::String lastPersisted;
    bool userEditing = false;
    double lastFailedOpenMs = -1.0e9;
    std::vector<double> restartTimes; // automatic restarts in the last few seconds
    bool reopening = false;
    bool attached = false;
    juce::String pendingError; // from audioDeviceError, handled on the message thread

    std::atomic<int> callbacks { 0 };
    std::shared_ptr<std::atomic<bool>> alive { std::make_shared<std::atomic<bool>> (true) };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioDeviceController)
};
} // namespace snds::audio
