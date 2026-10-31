#include "ui/TransportBar.h"

#include "ui/ErrorBanner.h"
#include "ui/SongPanel.h"

namespace snds
{
namespace
{
constexpr int rowGap = 6;
const juce::Colour warningColour (0xffd29922);
} // namespace

TransportBar::TransportBar()
{
    stemMixButton.setClickingTogglesState (true);
    originalButton.setClickingTogglesState (true);
    stemMixButton.setRadioGroupId (4711);
    originalButton.setRadioGroupId (4711);
    stemMixButton.setConnectedEdges (juce::Button::ConnectedOnRight);
    originalButton.setConnectedEdges (juce::Button::ConnectedOnLeft);

    position.setSliderStyle (juce::Slider::LinearHorizontal);
    position.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    position.setRange (0.0, 1.0, 0.0);

    timeLabel.setJustificationType (juce::Justification::centredRight);
    originalNote.setJustificationType (juce::Justification::centredLeft);
    originalNote.setFont (juce::FontOptions (13.0f));
    stateLabel.setJustificationType (juce::Justification::centredLeft);
    stateLabel.setFont (juce::FontOptions (13.0f));

    for (auto* component : std::initializer_list<juce::Component*> { &playButton,
                                                                     &stopButton,
                                                                     &position,
                                                                     &timeLabel,
                                                                     &loopToggle,
                                                                     &stemMixButton,
                                                                     &originalButton,
                                                                     &originalNote,
                                                                     &stateLabel,
                                                                     &fix,
                                                                     &moreInfo })
        addAndMakeVisible (component);

    fix.setVisible (false);
    moreInfo.setVisible (false);
    moreInfo.setTitle ("Problem details");
    fix.onClick = [this]
    {
        if (report && onRecovery && ! report->actions.empty())
            onRecovery (report->actions.front().kind);
    };
    moreInfo.onClick = [this]
    {
        if (report)
            showErrorDetails (*report, this);
    };

    playButton.onClick = [this]
    {
        if (controller != nullptr)
            controller->togglePlay();
    };
    stopButton.onClick = [this]
    {
        if (controller != nullptr)
            controller->stop();
    };
    loopToggle.onClick = [this]
    {
        if (controller != nullptr && ! updatingFromView)
            controller->setLooping (loopToggle.getToggleState());
    };
    stemMixButton.onClick = [this]
    {
        if (controller != nullptr && ! updatingFromView)
            controller->setOriginalSelected (false);
    };
    originalButton.onClick = [this]
    {
        if (controller != nullptr && ! updatingFromView)
            controller->setOriginalSelected (true);
    };

    position.onDragStart = [this] { dragging = true; };
    position.onDragEnd = [this]
    {
        dragging = false;
        commitSeek();
    };
    position.onValueChange = [this]
    {
        if (updatingFromView)
            return;

        // A drag only previews the time; a click or key press seeks at once.
        if (! dragging)
            commitSeek();
        else
            refresh();
    };

    refresh();
    startTimerHz (20);
}

TransportBar::~TransportBar()
{
    stopTimer();
}

void TransportBar::setController (PlaybackController* newController)
{
    controller = newController;
    refresh();
}

void TransportBar::commitSeek()
{
    if (controller != nullptr)
        controller->seekToFraction (position.getValue());
}

void TransportBar::timerCallback()
{
    refresh();
}

juce::String TransportBar::describeOriginal (const OriginalAvailability& availability)
{
    switch (availability.kind)
    {
        case OriginalAvailability::Kind::none:
        case OriginalAvailability::Kind::available:
            return {};
        case OriginalAvailability::Kind::checking:
            return "Checking the original...";
        case OriginalAvailability::Kind::missing:
            return "Original unavailable. Locate it to compare.";
        case OriginalAvailability::Kind::unusable:
            return "Original off: " + availability.reason;
    }

    return {};
}

juce::String TransportBar::describeState (const PlaybackView& view)
{
    switch (view.state)
    {
        case PlaybackState::unloaded:
            return "No song loaded.";
        case PlaybackState::preparing:
            return view.message.isNotEmpty() ? view.message : juce::String ("Preparing audio...");
        case PlaybackState::failed:
            return "Cannot play: " + view.message;
        default:
            break;
    }

    if (! view.deviceUsable())
        return "Audio device unavailable: "
             + (view.device.message.isNotEmpty() ? view.device.message : juce::String ("no output device is running."))
             + " Your position is kept.";

    const auto onFallback =
        view.device.state == audio::DeviceStatus::State::fallback
            ? " on " + view.device.deviceName + " (" + view.device.preferredDeviceName + " is not available)"
            : juce::String();

    if (view.state == PlaybackState::playing)
    {
        auto text = view.buffering ? juce::String ("Buffering...") : "Playing" + onFallback;

        if (view.underruns > 0)
            text << " - " << view.underruns << (view.underruns == 1 ? " dropout" : " dropouts");

        return text;
    }

    return (view.state == PlaybackState::paused ? juce::String ("Paused") : juce::String ("Ready")) + onFallback;
}

void TransportBar::refresh()
{
    PlaybackView view;

    if (controller != nullptr)
        view = controller->view();

    const juce::ScopedValueSetter<bool> guard (updatingFromView, true);

    const auto playing = view.state == PlaybackState::playing;
    playButton.setButtonText (playing ? "Pause" : "Play");
    playButton.setEnabled (view.canPlay());
    stopButton.setEnabled (view.canSeek());
    position.setEnabled (view.canSeek());
    loopToggle.setEnabled (view.canSeek());
    loopToggle.setToggleState (view.looping, juce::dontSendNotification);

    if (! dragging)
        position.setValue (view.fraction, juce::dontSendNotification);

    const auto shownSeconds = dragging ? position.getValue() * view.durationSeconds : view.positionSeconds;
    timeLabel.setText (view.hasSong() ? formatDuration (shownSeconds) + " / " + formatDuration (view.durationSeconds)
                                      : juce::String ("0:00 / 0:00"),
                       juce::dontSendNotification);

    const bool abEnabled = view.original.isAvailable() && view.canSeek();
    stemMixButton.setEnabled (abEnabled);
    originalButton.setEnabled (abEnabled);
    stemMixButton.setToggleState (! view.originalSelected, juce::dontSendNotification);
    originalButton.setToggleState (view.originalSelected, juce::dontSendNotification);

    const auto note = describeOriginal (view.original);

    if (originalNote.getText() != note)
        originalNote.setText (note, juce::dontSendNotification);

    originalNote.setColour (juce::Label::textColourId, warningColour);

    // A problem (song cannot be played, device lost) is shown from the shared report, with its first
    // action as a button and the rest behind Details; otherwise the plain state.
    report = reportFromPlayback (view);
    const bool problem = report.has_value() && report->severity != Severity::info;
    const auto text = report
                        ? (report->guidance.isNotEmpty() ? report->message + " " + report->guidance : report->message)
                        : describeState (view);
    fix.setVisible (report.has_value() && ! report->actions.empty());
    moreInfo.setVisible (report.has_value() && report->hasDetails());

    if (report && ! report->actions.empty() && fix.getButtonText() != report->actions.front().label)
        fix.setButtonText (report->actions.front().label);

    resized();

    if (text != stateLabel.getText())
        stateLabel.setText (text, juce::dontSendNotification);

    stateLabel.setColour (juce::Label::textColourId,
                          problem ? warningColour : findColour (juce::Label::textColourId).withAlpha (0.8f));
}

void TransportBar::resized()
{
    auto area = getLocalBounds();

    auto top = area.removeFromTop (30);
    playButton.setBounds (top.removeFromLeft (80));
    top.removeFromLeft (rowGap);
    stopButton.setBounds (top.removeFromLeft (70));
    top.removeFromLeft (rowGap);
    timeLabel.setBounds (top.removeFromRight (110));
    position.setBounds (top);

    area.removeFromTop (rowGap);
    auto middle = area.removeFromTop (28);
    loopToggle.setBounds (middle.removeFromLeft (70));
    middle.removeFromLeft (rowGap * 2);
    stemMixButton.setBounds (middle.removeFromLeft (90));
    originalButton.setBounds (middle.removeFromLeft (90));
    middle.removeFromLeft (rowGap * 2);
    originalNote.setBounds (middle);

    area.removeFromTop (rowGap - 2);
    auto stateRow = area.removeFromTop (22);

    if (moreInfo.isVisible())
        moreInfo.setBounds (stateRow.removeFromRight (92).reduced (0, 1));

    if (fix.isVisible())
    {
        stateRow.removeFromRight (6);
        fix.setBounds (stateRow.removeFromRight (150).reduced (0, 1));
    }

    stateLabel.setBounds (stateRow);
}
} // namespace snds
