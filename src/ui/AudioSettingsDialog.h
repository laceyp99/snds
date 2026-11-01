#pragma once

#include "audio/AudioDeviceController.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace snds
{
/** The output-device settings: JUCE's selector (device, sample rate, buffer size, channels) and
    a line saying what the controller sees. While it exists, the device shown is taken as the
    user's choice. Output only: SNDS has no inputs or MIDI. */
class AudioSettingsComponent final : public juce::Component, private juce::Timer
{
public:
    explicit AudioSettingsComponent (audio::AudioDeviceController& controller);
    ~AudioSettingsComponent() override;

    void resized() override;

    /** Status line text for a device status (also used by tests). */
    static juce::String describe (const audio::DeviceStatus& status);

private:
    void timerCallback() override;

    audio::AudioDeviceController& devices;
    juce::AudioDeviceSelectorComponent selector;
    juce::Label statusLabel;
    juce::TextButton reconnectButton { "Reconnect" };
    juce::String shown;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioSettingsComponent)
};

/** Opens the settings in a modeless dialog window and returns a handle to it. The dialog
    deletes itself when the user closes it (the handle then reads null); delete it through the
    handle to close it earlier, for example before the controller is destroyed. */
juce::Component::SafePointer<juce::Component> showAudioSettingsDialog (audio::AudioDeviceController& controller,
                                                                       juce::Component* relativeTo = nullptr);
} // namespace snds
