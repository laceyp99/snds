#include "audio/AudioDeviceController.h"

#include "app/Logging.h"

namespace snds::audio
{
namespace
{
bool sameStatus (const DeviceStatus& a, const DeviceStatus& b)
{
    return a.state == b.state && a.typeName == b.typeName && a.deviceName == b.deviceName
        && a.preferredDeviceName == b.preferredDeviceName && a.sampleRate == b.sampleRate
        && a.bufferSize == b.bufferSize && a.message == b.message;
}
} // namespace

AudioDeviceController::AudioDeviceController (PlaybackEngine& engineToUse, juce::AudioDeviceManager& managerToUse)
    : engine (engineToUse),
      manager (managerToUse)
{
}

AudioDeviceController::~AudioDeviceController()
{
    shutdown();
}

juce::String AudioDeviceController::initialise (const juce::String& savedStateXml)
{
    std::unique_ptr<juce::XmlElement> saved;

    if (savedStateXml.isNotEmpty())
        saved = juce::XmlDocument::parse (savedStateXml);

    if (saved != nullptr && saved->hasTagName ("DEVICESETUP"))
    {
        preferredType = saved->getStringAttribute ("deviceType");
        preferredOutput = saved->getStringAttribute ("audioOutputDeviceName");
        preferredStateXml = savedStateXml;
        lastPersisted = savedStateXml;
    }
    else
    {
        saved.reset();
    }

    manager.addChangeListener (this);
    const auto error = manager.initialise (0, 2, saved.get(), true);
    manager.addAudioCallback (this); // starts the engine at once when a device is already open
    attached = true;
    evaluate();

    if (status.isUsable())
        return {};

    return error.isNotEmpty() ? error : status.message;
}

void AudioDeviceController::shutdown()
{
    alive->store (false);

    if (attached)
    {
        manager.removeAudioCallback (this);
        manager.removeChangeListener (this);
        attached = false;
        engine.deviceStopped();
    }
}

void AudioDeviceController::setUserEditing (bool isEditing)
{
    userEditing = isEditing;

    if (attached)
        evaluate();
}

void AudioDeviceController::reconnect()
{
    if (attached)
    {
        restartTimes.clear();
        lastFailedOpenMs = -1.0e9;
        if (preferredIsAvailable() && ! activeIsPreferred())
            openPreferred();
        else
            evaluate();
    }
}

//==============================================================================
void AudioDeviceController::audioDeviceIOCallbackWithContext (const float* const*,
                                                              int,
                                                              float* const* output,
                                                              int numOutputChannels,
                                                              int numSamples,
                                                              const juce::AudioIODeviceCallbackContext&)
{
    callbacks.fetch_add (1, std::memory_order_relaxed);
    engine.process (output, numOutputChannels, numSamples);
}

void AudioDeviceController::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    // A new device, rate or buffer size: rebuild the resampler; the position is kept.
    engine.prepare (device->getCurrentSampleRate(), std::max (1, device->getCurrentBufferSizeSamples()));
}

void AudioDeviceController::audioDeviceStopped()
{
    engine.deviceStopped();
    const auto flag = alive;

    juce::MessageManager::callAsync (
        [this, flag]
        {
            if (flag->load() && attached)
                evaluate();
        });
}

void AudioDeviceController::audioDeviceError (const juce::String& errorMessage)
{
    engine.deviceStopped();
    const auto flag = alive;

    juce::MessageManager::callAsync (
        [this, flag, errorMessage]
        {
            if (! flag->load() || ! attached)
                return;

            logWarning ("Audio device error: " + errorMessage);
            pendingError = errorMessage;
            evaluate();
        });
}

void AudioDeviceController::changeListenerCallback (juce::ChangeBroadcaster*)
{
    evaluate();
}

//==============================================================================
bool AudioDeviceController::activeIsPreferred() const
{
    const auto* device = manager.getCurrentAudioDevice();

    if (device == nullptr)
        return false;

    const auto setup = manager.getAudioDeviceSetup();
    return (preferredType.isEmpty() || preferredType == manager.getCurrentAudioDeviceType())
        && setup.outputDeviceName == preferredOutput;
}

bool AudioDeviceController::preferredIsAvailable() const
{
    if (preferredOutput.isEmpty())
        return false;

    for (auto* type : manager.getAvailableDeviceTypes())
        if ((preferredType.isEmpty() || type->getTypeName() == preferredType)
            && type->getDeviceNames (false).contains (preferredOutput))
            return true;

    return false;
}

void AudioDeviceController::rememberPreferred()
{
    const auto setup = manager.getAudioDeviceSetup();
    preferredType = manager.getCurrentAudioDeviceType();
    preferredOutput = setup.outputDeviceName;

    // createStateXml() is empty while the system default has never been chosen explicitly.
    if (const auto xml = manager.createStateXml())
    {
        preferredStateXml = xml->toString();

        if (preferredStateXml != lastPersisted)
        {
            lastPersisted = preferredStateXml;

            if (onPersist)
                onPersist (preferredStateXml);
        }
    }
}

void AudioDeviceController::openPreferred()
{
    if (reopening)
        return;

    reopening = true;
    logInfo ("Audio: reopening " + preferredOutput);
    juce::String error;

    if (preferredType.isNotEmpty() && manager.getCurrentAudioDeviceType() != preferredType)
        manager.setCurrentAudioDeviceType (preferredType, true);

    auto setup = manager.getAudioDeviceSetup();
    setup.outputDeviceName = preferredOutput;
    setup.inputDeviceName = {};
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = true;

    if (const auto saved = juce::XmlDocument::parse (preferredStateXml))
    {
        setup.sampleRate = saved->getDoubleAttribute ("audioDeviceRate", 0.0);
        setup.bufferSize = saved->getIntAttribute ("audioDeviceBufferSize", 0);
    }

    error = manager.setAudioDeviceSetup (setup, true);
    reopening = false;

    if (error.isNotEmpty())
        lastFailedOpenMs = juce::Time::getMillisecondCounterHiRes();

    if (error.isNotEmpty())
    {
        logWarning ("Audio: could not reopen " + preferredOutput + ": " + error);
        pendingError = error;
    }

    evaluate();
}

void AudioDeviceController::evaluate()
{
    if (reopening)
        return;

    DeviceStatus next;
    next.preferredDeviceName = preferredOutput;
    auto* device = manager.getCurrentAudioDevice();

    const auto now = juce::Time::getMillisecondCounterHiRes();
    std::erase_if (restartTimes, [now] (double t) { return now - t > 5000.0; });
    const bool mayRetry = ! userEditing && now - lastFailedOpenMs > 1000.0 && restartTimes.size() < 3;

    if (device == nullptr)
    {
        // Every device went away, and one is listed again: bring the preferred one back.
        if (mayRetry && preferredIsAvailable())
        {
            restartTimes.push_back (now);
            openPreferred();
            return;
        }

        next.state = preferredIsAvailable() ? DeviceStatus::State::failed : DeviceStatus::State::lost;
        next.message = pendingError.isNotEmpty() ? pendingError
                     : next.state == DeviceStatus::State::lost
                         ? (preferredOutput.isEmpty() ? juce::String ("No audio output device is available.")
                                                      : preferredOutput + " is not available.")
                         : "The audio device stopped.";
        publish (next);
        return;
    }

    // The device is open but stopped (a driver reset, say): say so, then restart it. The
    // transport was stopped when the device stopped.
    if (! device->isPlaying() && mayRetry && preferredIsAvailable() && activeIsPreferred())
    {
        next.state = DeviceStatus::State::failed;
        next.deviceName = manager.getAudioDeviceSetup().outputDeviceName;
        next.message = pendingError.isNotEmpty() ? pendingError : "The audio device stopped.";
        publish (next);
        pendingError = {};
        restartTimes.push_back (now);
        manager.closeAudioDevice();
        openPreferred();
        return;
    }

    next.typeName = manager.getCurrentAudioDeviceType();
    next.deviceName = manager.getAudioDeviceSetup().outputDeviceName;
    next.sampleRate = device->getCurrentSampleRate();
    next.bufferSize = device->getCurrentBufferSizeSamples();

    if (userEditing || preferredOutput.isEmpty())
        rememberPreferred();

    next.preferredDeviceName = preferredOutput;

    if (activeIsPreferred())
    {
        rememberPreferred(); // rate, buffer size or channel changes made in the dialog
        next.state = DeviceStatus::State::running;
        pendingError = {};
    }
    else if (preferredIsAvailable())
    {
        openPreferred(); // it came back
        return;
    }
    else
    {
        next.state = DeviceStatus::State::fallback;
        next.message = preferredOutput + " is not available; using " + next.deviceName + ".";
    }

    publish (next);
}

void AudioDeviceController::publish (DeviceStatus next)
{
    if (sameStatus (status, next))
        return;

    status = std::move (next);
    logInfo ("Audio: "
             + juce::String (status.state == DeviceStatus::State::running    ? "running"
                             : status.state == DeviceStatus::State::fallback ? "fallback"
                             : status.state == DeviceStatus::State::lost     ? "lost"
                             : status.state == DeviceStatus::State::failed   ? "failed"
                                                                             : "not started")
             + " on " + (status.deviceName.isEmpty() ? juce::String ("no device") : status.deviceName) + " ("
             + juce::String (status.sampleRate) + " Hz, " + juce::String (status.bufferSize) + " frames)"
             + (status.message.isEmpty() ? juce::String() : juce::String (": ") + status.message));

    if (onStatusChanged)
        onStatusChanged (status);
}
} // namespace snds::audio
