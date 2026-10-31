#pragma once

// A simulated audio output device (and device type) plus message-queue helpers, so the real
// AudioDeviceManager and AudioDeviceController can be driven without audio hardware: the test
// plays the operating system's audio thread by calling FakeDevice::runBlock().

#include "PlaybackTestKit.h"
#include "audio/AudioDeviceController.h"
#include "audio/PlaybackEngine.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>
#include <utility>
#include <vector>

namespace snds::test
{
using namespace snds::audio;

//==============================================================================
/** A simulated output device. The test plays the part of the operating system's audio thread by
    calling runBlock(); nothing here touches real audio hardware. */
class FakeDevice final : public juce::AudioIODevice
{
public:
    FakeDevice (const juce::String& name, const juce::String& typeName, std::function<void (FakeDevice*)> onDestroyed)
        : juce::AudioIODevice (name, typeName),
          destroyed (std::move (onDestroyed))
    {
    }

    ~FakeDevice() override { destroyed (this); }

    juce::StringArray getOutputChannelNames() override { return { "Left", "Right" }; }
    juce::StringArray getInputChannelNames() override { return {}; }
    juce::Array<double> getAvailableSampleRates() override { return { 44100.0, 48000.0, 96000.0 }; }
    juce::Array<int> getAvailableBufferSizes() override { return { 128, 256, 512, 1024 }; }
    int getDefaultBufferSize() override { return 512; }

    juce::String open (const juce::BigInteger&, const juce::BigInteger&, double sampleRate, int bufferSize) override
    {
        rate = sampleRate > 0.0 ? sampleRate : 48000.0;
        buffer = bufferSize > 0 ? bufferSize : 512;
        opened = true;
        return {};
    }

    void close() override
    {
        stop();
        opened = false;
    }
    bool isOpen() override { return opened; }

    void start (juce::AudioIODeviceCallback* newCallback) override
    {
        callback = newCallback;
        running = true;
        callback->audioDeviceAboutToStart (this);
    }

    void stop() override
    {
        if (running)
        {
            running = false;
            auto* stopped = std::exchange (callback, nullptr);
            stopped->audioDeviceStopped();
        }
    }

    bool isPlaying() override { return running; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return buffer; }
    double getCurrentSampleRate() override { return rate; }
    int getCurrentBitDepth() override { return 32; }

    juce::BigInteger getActiveOutputChannels() const override
    {
        juce::BigInteger channels;
        channels.setRange (0, 2, true);
        return channels;
    }

    juce::BigInteger getActiveInputChannels() const override { return {}; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }

    //==============================================================================
    /** One audio callback of `frames` frames; returns false if the device is not running. */
    bool runBlock (int frames, std::vector<float>& left, std::vector<float>& right)
    {
        left.assign (static_cast<size_t> (frames), 1.0f);
        right.assign (static_cast<size_t> (frames), 1.0f);

        if (callback == nullptr)
            return false;

        float* channels[2] = { left.data(), right.data() };
        callback->audioDeviceIOCallbackWithContext (nullptr, 0, channels, 2, frames, {});
        return true;
    }

    void simulateError (const juce::String& message)
    {
        if (callback != nullptr)
            callback->audioDeviceError (message);
    }

private:
    std::function<void (FakeDevice*)> destroyed;
    juce::AudioIODeviceCallback* callback = nullptr;
    bool opened = false;
    bool running = false;
    double rate = 48000.0;
    int buffer = 512;
};

class FakeDeviceType final : public juce::AudioIODeviceType
{
public:
    FakeDeviceType()
        : juce::AudioIODeviceType ("Fake Audio")
    {
    }

    void scanForDevices() override {}
    juce::StringArray getDeviceNames (bool wantInputNames = false) const override
    {
        return wantInputNames ? juce::StringArray() : names;
    }
    int getDefaultDeviceIndex (bool) const override { return 0; }

    int getIndexOfDevice (juce::AudioIODevice* device, bool asInput) const override
    {
        return asInput || device == nullptr ? -1 : names.indexOf (device->getName());
    }

    bool hasSeparateInputsAndOutputs() const override { return true; }

    juce::AudioIODevice* createDevice (const juce::String& outputName, const juce::String&) override
    {
        if (! names.contains (outputName))
            return nullptr;

        auto* device = new FakeDevice (outputName,
                                       getTypeName(),
                                       [this] (FakeDevice* gone)
                                       {
                                           if (live == gone)
                                               live = nullptr;
                                       });
        live = device;
        return device;
    }

    /** The operating system lists a device (and tells the manager). */
    void plugIn (const juce::String& name)
    {
        names.addIfNotAlreadyThere (name);
        callDeviceChangeListeners();
    }

    /** The operating system removes a device: the open one reports an error first, as WASAPI does. */
    void unplug (const juce::String& name)
    {
        if (live != nullptr && live->getName() == name)
            live->simulateError (name + " was removed");

        names.removeString (name);
        callDeviceChangeListeners();
    }

    void setInitialDevices (const juce::StringArray& list) { names = list; }
    FakeDevice* current() const { return live; }

private:
    juce::StringArray names;
    FakeDevice* live = nullptr;
};

//==============================================================================
/** Message-thread plumbing for tests: JUCE delivers change messages and callAsync through the
    message queue, so the test pumps it. */
struct MessageLoop
{
    MessageLoop() { juce::MessageManager::getInstance()->setCurrentThreadAsMessageThread(); }

    /** Dispatches pending Win32 messages (JUCE's message window lives on this thread) for `ms`. */
    static void pump (int ms = 60)
    {
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + ms;

        do
        {
            MSG message;

            while (PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage (&message);
                DispatchMessageW (&message);
            }

            juce::Thread::sleep (2);
        } while (juce::Time::getMillisecondCounterHiRes() < deadline);
    }

    template <typename Predicate>
    static bool pumpUntil (Predicate&& done, int timeoutMs = 3000)
    {
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;

        while (juce::Time::getMillisecondCounterHiRes() < deadline)
        {
            pump (10);

            if (done())
                return true;
        }

        return done();
    }
};

/** One engine + manager + controller wired to the fake device type. */
struct Rig
{
    explicit Rig (const juce::StringArray& devices = { "Speakers", "Headphones" })
        : controller (engine, manager)
    {
        auto type = std::make_unique<FakeDeviceType>();
        fake = type.get();
        fake->setInitialDevices (devices);
        manager.addAudioDeviceType (std::move (type));
        controller.onPersist = [this] (const juce::String& xml) { persisted.add (xml); };
        controller.onStatusChanged = [this] (const DeviceStatus& status) { history.add (status.state); };
    }

    ~Rig() { controller.shutdown(); }

    bool runBlocks (int count,
                    int frames,
                    std::vector<float>* lastLeft = nullptr,
                    std::vector<float>* lastRight = nullptr)
    {
        std::vector<float> left, right;

        for (int i = 0; i < count; ++i)
            if (fake->current() == nullptr || ! fake->current()->runBlock (frames, left, right))
                return false;

        if (lastLeft != nullptr)
            *lastLeft = left;

        if (lastRight != nullptr)
            *lastRight = right;

        return true;
    }

    Rendered render (int frames, int block)
    {
        Rendered rendered;
        std::vector<float> left, right;

        for (int done = 0; done < frames; done += block)
        {
            engine.waitForReadAhead (block, 5000);
            fake->current()->runBlock (block, left, right);
            rendered.left.insert (rendered.left.end(), left.begin(), left.end());
            rendered.right.insert (rendered.right.end(), right.begin(), right.end());
        }

        return rendered;
    }

    PlaybackEngine engine;
    juce::AudioDeviceManager manager;
    AudioDeviceController controller;
    FakeDeviceType* fake = nullptr;
    juce::StringArray persisted;
    juce::Array<DeviceStatus::State> history;
};

inline juce::String xmlFor (const juce::String& device, double rate = 48000.0, int buffer = 512)
{
    return "<DEVICESETUP deviceType=\"Fake Audio\" audioOutputDeviceName=\"" + device
         + "\" audioInputDeviceName=\"\" audioDeviceRate=\"" + juce::String (rate) + "\" audioDeviceBufferSize=\""
         + juce::String (buffer) + "\" />";
}
} // namespace snds::test
