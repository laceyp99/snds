#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "FakeAudioDevice.h"
#include "PlaybackTestKit.h"
#include "TestHelpers.h"
#include "app/AudioService.h"
#include "app/Settings.h"
#include "audio/AudioDeviceController.h"
#include "ui/AudioSettingsDialog.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <functional>
#include <vector>

namespace snds
{
namespace
{
using namespace snds::audio;
using namespace snds::test;

constexpr int songRate = 44100;
const int ramp = rampFrames (44100.0);

//==============================================================================
class DeviceSettingsTests final : public juce::UnitTest
{
public:
    DeviceSettingsTests()
        : juce::UnitTest ("DeviceSettings", "snds")
    {
    }

    void runTest() override
    {
        ScopedTempDirectory temp;
        const auto file = temp.child ("settings.json");

        beginTest ("DeviceSettings: the audio device choice is stored, survives a reload, and can be cleared");
        {
            Settings settings (file);
            expectEquals (settings.getAudioDeviceState(), juce::String());
            settings.setForceCpu (true);
            settings.setAudioDeviceState (xmlFor ("Headphones", 44100.0, 256));
            expect (settings.save().wasOk());

            Settings reloaded (file);
            expect (reloaded.load().warnings.isEmpty());
            expectEquals (reloaded.getAudioDeviceState(), xmlFor ("Headphones", 44100.0, 256));
            expect (reloaded.getForceCpu(), "other settings are untouched");

            reloaded.setAudioDeviceState ({});
            expect (reloaded.save().wasOk());
            Settings cleared (file);
            cleared.load();
            expectEquals (cleared.getAudioDeviceState(), juce::String());
        }

        beginTest ("DeviceSettings: an invalid stored value is ignored with a warning");
        {
            file.replaceWithText (R"({"format":"snds-settings","version":1,"audioDevice":42,"futureKey":"kept"})");
            Settings settings (file);
            const auto result = settings.load();
            expect (result.warnings.size() == 1 && result.warnings[0].contains ("audioDevice"));
            expectEquals (settings.getAudioDeviceState(), juce::String());
            expect (settings.save().wasOk());
            expect (file.loadFileAsString().contains ("futureKey"), "unknown keys are kept");
        }
    }
};

DeviceSettingsTests deviceSettingsTests;

//==============================================================================
class DeviceFlowTests final : public juce::UnitTest
{
public:
    DeviceFlowTests()
        : juce::UnitTest ("DeviceFlow", "snds")
    {
    }

    void runTest() override
    {
        MessageLoop loop;
        testStartAndPlay();
        testPersistence();
        testRateAndBufferChange();
        testLossAndRecovery();
        testErrorWithoutRemoval();
        testNoDeviceAtAll();
        testSavedDeviceMissingAtStart();
        testDialog();
        testRealDeviceSmoke();
    }

private:
    static constexpr std::int64_t songLength = songRate * 20;

    static bool loadSong (PlaybackEngine& engine) { return engine.load (makeMarkerSources (songRate, songLength)); }

    void testStartAndPlay()
    {
        beginTest ("DeviceFlow: the engine plays through the device manager's callback");
        Rig rig;
        expectEquals (rig.controller.initialise ({}), juce::String());
        expect (rig.controller.getStatus().state == DeviceStatus::State::running);
        expectEquals (rig.controller.getStatus().deviceName, juce::String ("Speakers"));
        expectEquals (rig.controller.getStatus().sampleRate, 48000.0);
        expectEquals (rig.controller.getStatus().bufferSize, 512);
        expectEquals (rig.engine.getDeviceSampleRate(), 48000.0);

        // Stopped engine: the device runs and the output is silence.
        std::vector<float> left, right;
        expect (rig.runBlocks (3, 512, &left, &right));
        expect (rig.controller.getCallbackCount() >= 3);
        expect (std::all_of (left.begin(), left.end(), [] (float v) { return v == 0.0f; }));

        expect (loadSong (rig.engine));
        rig.engine.play();
        const auto rendered = rig.render (4000, 512);
        const auto start =
            std::find_if (rendered.left.begin(), rendered.left.end(), [] (float v) { return v != 0.0f; });
        expect (start != rendered.left.end(), "sound comes out of the device callback");
        expect (rig.engine.getPositionFrames() > 0);
    }

    void testPersistence()
    {
        beginTest ("DeviceFlow: choosing a device in the dialog is persisted and restored");
        juce::String saved;
        {
            Rig rig;
            expectEquals (rig.controller.initialise ({}), juce::String());
            expectEquals (rig.persisted.size(), 0); // the system default has not been chosen explicitly

            rig.controller.setUserEditing (true); // the settings dialog is open
            auto setup = rig.manager.getAudioDeviceSetup();
            setup.outputDeviceName = "Headphones";
            setup.sampleRate = 96000.0;
            setup.bufferSize = 256;
            expectEquals (rig.manager.setAudioDeviceSetup (setup, true), juce::String());
            MessageLoop::pump();
            rig.controller.setUserEditing (false);

            expect (rig.persisted.size() >= 1);
            saved = rig.persisted[rig.persisted.size() - 1];
            expect (saved.contains ("Headphones") && saved.contains ("96000") && saved.contains ("256"), saved);
            expect (rig.controller.getStatus().state == DeviceStatus::State::running);
            expectEquals (rig.controller.getStatus().preferredDeviceName, juce::String ("Headphones"));
        }

        Rig second;
        expectEquals (second.controller.initialise (saved), juce::String());
        expectEquals (second.controller.getStatus().deviceName, juce::String ("Headphones"));
        expectEquals (second.controller.getStatus().sampleRate, 96000.0);
        expectEquals (second.controller.getStatus().bufferSize, 256);
        expectEquals (second.engine.getDeviceSampleRate(), 96000.0);
        expectEquals (second.persisted.size(), 0, "restoring does not rewrite the setting");
    }

    void testRateAndBufferChange()
    {
        beginTest ("DeviceFlow: a new device rate stops the transport at its position and re-prepares the resampler");
        Rig rig;
        expectEquals (rig.controller.initialise (xmlFor ("Speakers", 48000.0, 512)), juce::String());
        expect (loadSong (rig.engine));
        rig.engine.play();
        rig.render (12000, 512);
        const auto before = rig.engine.getPositionFrames();
        expect (before > 5000);

        rig.controller.setUserEditing (true);
        auto setup = rig.manager.getAudioDeviceSetup();
        setup.sampleRate = 96000.0;
        setup.bufferSize = 256;
        expectEquals (rig.manager.setAudioDeviceSetup (setup, true), juce::String());
        MessageLoop::pump();
        rig.controller.setUserEditing (false);

        expectEquals (rig.engine.getDeviceSampleRate(), 96000.0);
        expect (! rig.engine.isPlaying(), "a device change stops the transport");
        expectEquals (static_cast<juce::int64> (rig.engine.getPositionFrames()), static_cast<juce::int64> (before));

        rig.engine.play();
        const auto rendered = rig.render (9600, 200);
        const auto advanced = rig.engine.getPositionFrames() - before;
        // 9600 device frames at 96 kHz are 4410 source frames (the first few belong to the fade-in).
        expect (std::abs (advanced - 4410) <= 3, "advanced " + juce::String (advanced));
        expect (std::any_of (rendered.left.begin(), rendered.left.end(), [] (float v) { return v != 0.0f; }));
        expectEquals (rig.engine.getUnderrunCount(), 0);
    }

    void testLossAndRecovery()
    {
        beginTest ("DeviceFlow: losing the preferred device stops safely; its return restores the same position");
        Rig rig;
        expectEquals (rig.controller.initialise (xmlFor ("Headphones", 44100.0)), juce::String());
        expectEquals (rig.controller.getStatus().deviceName, juce::String ("Headphones"));
        expect (loadSong (rig.engine));
        rig.engine.play();
        rig.render (30000, 512);
        const auto before = rig.engine.getPositionFrames();
        expect (rig.engine.isPlaying() && before > 20000);

        rig.fake->unplug ("Headphones"); // the OS drops it; JUCE falls back to the remaining device
        MessageLoop::pumpUntil ([&] { return rig.controller.getStatus().state == DeviceStatus::State::fallback; });

        expect (rig.controller.getStatus().state == DeviceStatus::State::fallback, "status: fallback");
        expectEquals (rig.controller.getStatus().deviceName, juce::String ("Speakers"));
        expect (rig.controller.getStatus().message.contains ("Headphones"), rig.controller.getStatus().message);
        expect (! rig.engine.isPlaying() && ! rig.engine.isAudible(), "the transport stopped");
        expectEquals (static_cast<juce::int64> (rig.engine.getPositionFrames()), static_cast<juce::int64> (before));
        expect (std::none_of (rig.persisted.begin(),
                              rig.persisted.end(),
                              [] (const juce::String& xml) { return xml.contains ("Speakers"); }),
                "the fallback device is not stored as the user's choice");

        // Silent on the fallback, position still kept, and a seek still works while the device is away.
        std::vector<float> left, right;
        expect (rig.runBlocks (4, 512, &left, &right));
        expect (std::all_of (left.begin(), left.end(), [] (float v) { return v == 0.0f; }));
        expectEquals (static_cast<juce::int64> (rig.engine.getPositionFrames()), static_cast<juce::int64> (before));

        rig.fake->plugIn ("Headphones");
        MessageLoop::pumpUntil ([&] { return rig.controller.getStatus().state == DeviceStatus::State::running; });
        expect (rig.controller.getStatus().state == DeviceStatus::State::running, "status: running again");
        expectEquals (rig.controller.getStatus().deviceName, juce::String ("Headphones"));
        expect (! rig.engine.isPlaying(), "recovery does not start the music by itself");
        expectEquals (static_cast<juce::int64> (rig.engine.getPositionFrames()), static_cast<juce::int64> (before));

        rig.engine.play();
        const auto rendered = rig.render (6000, 512);
        const auto firstFull =
            std::find (rendered.left.begin(), rendered.left.end(), expectedUnitySum (before + ramp - 1, 0));
        expect (firstFull != rendered.left.end(), "playback continues from exactly where it stopped");

        if (firstFull != rendered.left.end())
        {
            const auto index = firstFull - rendered.left.begin();
            bool contiguous = true;

            for (int k = 0; k < 2000 && contiguous; ++k)
                contiguous =
                    rendered.left[static_cast<size_t> (index + k)] == expectedUnitySum (before + ramp - 1 + k, 0);

            expect (contiguous);
        }
    }

    void testErrorWithoutRemoval()
    {
        beginTest ("DeviceFlow: a device that stops by itself is reported, then restarted, with the position kept");
        Rig rig;
        expectEquals (rig.controller.initialise (xmlFor ("Headphones", 44100.0)), juce::String());
        expect (loadSong (rig.engine));
        rig.engine.play();
        rig.render (9000, 512);
        const auto before = rig.engine.getPositionFrames();
        const auto callbacksBefore = rig.controller.getCallbackCount();
        rig.history.clear();

        rig.fake->current()->stop(); // the driver stops the stream; the device stays listed
        MessageLoop::pumpUntil ([&] { return rig.history.contains (DeviceStatus::State::running); });

        expect (rig.history.contains (DeviceStatus::State::failed), "the stop was reported");

        expect (rig.controller.getStatus().state == DeviceStatus::State::running, "and the device was reopened");
        expect (! rig.engine.isPlaying());
        expectEquals (static_cast<juce::int64> (rig.engine.getPositionFrames()), static_cast<juce::int64> (before));
        expect (rig.runBlocks (2, 512));
        expect (rig.controller.getCallbackCount() > callbacksBefore);
    }

    void testNoDeviceAtAll()
    {
        beginTest (
            "DeviceFlow: with every device gone the status says so, seeks still work, and a device brings audio back");
        Rig rig ({ "Headphones" });
        expectEquals (rig.controller.initialise (xmlFor ("Headphones", 44100.0)), juce::String());
        expect (loadSong (rig.engine));
        rig.engine.play();
        rig.render (9000, 512);

        rig.fake->unplug ("Headphones");
        MessageLoop::pumpUntil ([&] { return rig.controller.getStatus().state == DeviceStatus::State::lost; });
        expect (rig.controller.getStatus().state == DeviceStatus::State::lost);
        expect (rig.controller.getStatus().message.contains ("Headphones"));
        expect (! rig.controller.getStatus().isUsable());
        expect (! rig.engine.isPlaying());

        rig.engine.seek (123456);
        expectEquals (static_cast<juce::int64> (rig.engine.getPositionFrames()), static_cast<juce::int64> (123456));

        rig.fake->plugIn ("Headphones");
        MessageLoop::pumpUntil ([&] { return rig.controller.getStatus().state == DeviceStatus::State::running; });
        expect (rig.controller.getStatus().state == DeviceStatus::State::running);
        rig.engine.play();
        const auto rendered = rig.render (6000, 512);
        expect (std::find (rendered.left.begin(), rendered.left.end(), expectedUnitySum (123456 + ramp - 1, 0))
                    != rendered.left.end(),
                "plays from the position chosen while the device was away");
    }

    void testSavedDeviceMissingAtStart()
    {
        beginTest ("DeviceFlow: a saved device that is not there at start falls back, keeps the saved choice, and "
                   "returns to it");
        Rig rig ({ "Speakers" });
        const auto saved = xmlFor ("Headphones");
        expectEquals (rig.controller.initialise (saved), juce::String());
        expect (rig.controller.getStatus().state == DeviceStatus::State::fallback);
        expectEquals (rig.controller.getStatus().deviceName, juce::String ("Speakers"));
        expectEquals (rig.controller.getStatus().preferredDeviceName, juce::String ("Headphones"));
        expectEquals (rig.persisted.size(), 0, "the saved choice is not overwritten by the fallback");

        rig.fake->plugIn ("Headphones");
        MessageLoop::pumpUntil ([&] { return rig.controller.getStatus().state == DeviceStatus::State::running; });
        expectEquals (rig.controller.getStatus().deviceName, juce::String ("Headphones"));
    }

    void testDialog()
    {
        beginTest ("DeviceFlow: the settings dialog marks the shown device as the choice and describes the state");
        Rig rig;
        expectEquals (rig.controller.initialise (xmlFor ("Speakers")), juce::String());
        {
            AudioSettingsComponent dialog (rig.controller);
            auto setup = rig.manager.getAudioDeviceSetup();
            setup.outputDeviceName = "Headphones";
            expectEquals (rig.manager.setAudioDeviceSetup (setup, true), juce::String());
            MessageLoop::pump();
            expectEquals (rig.controller.getStatus().preferredDeviceName, juce::String ("Headphones"));

            if (const auto snapshotDir = juce::SystemStats::getEnvironmentVariable ("SNDS_SNAPSHOT_DIR", {});
                snapshotDir.isNotEmpty())
            {
                // Opt-in: render the dialog to a PNG for review (no desktop window is involved).
                const auto image = dialog.createComponentSnapshot (dialog.getLocalBounds(), true, 1.0f);
                juce::PNGImageFormat png;
                juce::FileOutputStream out (juce::File (snapshotDir).getChildFile ("audio-settings.png"));

                if (out.openedOk())
                {
                    out.setPosition (0);
                    out.truncate();
                    png.writeImageToStream (image, out);
                }
            }
        }

        DeviceStatus status;
        status.state = DeviceStatus::State::running;
        status.deviceName = "Speakers";
        status.sampleRate = 44100.0;
        expect (AudioSettingsComponent::describe (status).contains ("Speakers at 44100 Hz"));
        status.state = DeviceStatus::State::lost;
        status.message = "Speakers is not available.";
        expect (AudioSettingsComponent::describe (status).contains ("position is kept"));
    }

    void testRealDeviceSmoke()
    {
        beginTest ("DeviceFlow: the real default device opens and runs silently (opt-in: SNDS_REAL_DEVICE=1)");

        if (juce::SystemStats::getEnvironmentVariable ("SNDS_REAL_DEVICE", {}) != "1")
            return;

        // Nothing is loaded in the engine, so the callback writes only silence to the device.
        AudioService service (nullptr);
        const auto error = service.start();
        MessageLoop::pump (400);
        const auto status = service.devices().getStatus();
        logMessage ("real device: " + status.deviceName + " [" + status.typeName + "] "
                    + juce::String (status.sampleRate) + " Hz, " + juce::String (status.bufferSize)
                    + " frames, callbacks " + juce::String (service.devices().getCallbackCount()));
        expectEquals (error, juce::String());
        expect (status.isUsable());
        expect (service.devices().getCallbackCount() > 0, "the device called back");
        expect (! service.engine().isPlaying());
    }
};

DeviceFlowTests deviceFlowTests;
} // namespace
} // namespace snds
