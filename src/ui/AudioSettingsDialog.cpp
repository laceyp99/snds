#include "ui/AudioSettingsDialog.h"

namespace snds
{
AudioSettingsComponent::AudioSettingsComponent (audio::AudioDeviceController& controller)
    : devices (controller),
      selector (controller.getManager(), 0, 0, 1, 2, false, false, true, false)
{
    devices.setUserEditing (true);

    addAndMakeVisible (selector);
    addAndMakeVisible (statusLabel);
    addAndMakeVisible (reconnectButton);
    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setMinimumHorizontalScale (1.0f);
    reconnectButton.onClick = [this] { devices.reconnect(); };

    setSize (520, 420);
    startTimerHz (4);
    timerCallback();
}

AudioSettingsComponent::~AudioSettingsComponent()
{
    devices.setUserEditing (false);
}

juce::String AudioSettingsComponent::describe (const audio::DeviceStatus& status)
{
    using State = audio::DeviceStatus::State;

    switch (status.state)
    {
        case State::notStarted:
            return "Audio is not started.";
        case State::running:
            return "Playing through " + status.deviceName + " at " + juce::String (juce::roundToInt (status.sampleRate))
                 + " Hz.";
        case State::fallback:
            return status.message;
        case State::lost:
        case State::failed:
            return status.message + " Playback is stopped; your position is kept.";
    }

    return {};
}

void AudioSettingsComponent::timerCallback()
{
    const auto text = describe (devices.getStatus());

    if (text != shown)
    {
        shown = text;
        statusLabel.setText (text, juce::dontSendNotification);
    }

    const auto state = devices.getStatus().state;
    reconnectButton.setVisible (state == audio::DeviceStatus::State::lost || state == audio::DeviceStatus::State::failed
                                || state == audio::DeviceStatus::State::fallback);
}

void AudioSettingsComponent::resized()
{
    auto area = getLocalBounds().reduced (8);
    auto bottom = area.removeFromBottom (28);
    reconnectButton.setBounds (bottom.removeFromRight (100));
    statusLabel.setBounds (bottom);
    area.removeFromBottom (6);
    selector.setBounds (area);
}

namespace
{
class AudioSettingsWindow final : public juce::DialogWindow
{
public:
    explicit AudioSettingsWindow (audio::AudioDeviceController& controller)
        : DialogWindow (
              "Audio settings",
              juce::LookAndFeel::getDefaultLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId),
              true)
    {
        setContentOwned (new AudioSettingsComponent (controller), true);
        setUsingNativeTitleBar (true);
        setResizable (false, false);
    }

    void closeButtonPressed() override { delete this; }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioSettingsWindow)
};
} // namespace

juce::Component::SafePointer<juce::Component> showAudioSettingsDialog (audio::AudioDeviceController& controller,
                                                                       juce::Component* relativeTo)
{
    auto* window = new AudioSettingsWindow (controller);
    window->centreAroundComponent (relativeTo, window->getWidth(), window->getHeight());
    window->setVisible (true);
    window->toFront (true);
    return window;
}
} // namespace snds
