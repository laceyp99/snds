#include "ui/ReadinessPanel.h"

#include "app/ErrorReport.h"

namespace snds
{
namespace
{
constexpr int padding = 12;
constexpr int summaryHeight = 64;
constexpr int detailsHeight = 200;
constexpr int dotSize = 12;
} // namespace

ReadinessPanel::ReadinessPanel()
{
    headline.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    headline.setMinimumHorizontalScale (1.0f);
    guidance.setFont (juce::FontOptions (14.0f));
    guidance.setColour (juce::Label::textColourId, findColour (juce::Label::textColourId).withAlpha (0.75f));

    details.setMultiLine (true, false);
    details.setReadOnly (true);
    details.setScrollbarsShown (true);
    details.setCaretVisible (false);
    details.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));

    detailsButton.onClick = [this] { setDetailsShown (! detailsShown); };
    recheckButton.onClick = [this]
    {
        if (onRecheck != nullptr)
            onRecheck();
    };

    for (auto* child : std::initializer_list<juce::Component*> { &headline, &guidance, &detailsButton, &recheckButton })
        addAndMakeVisible (child);

    addChildComponent (details);
    setReadiness ({});
}

void ReadinessPanel::setReadiness (const Readiness& readiness)
{
    const auto checking = readiness.state == Readiness::State::checking;

    if (checking)
        checkStartedMs = juce::Time::getMillisecondCounterHiRes();

    current = readiness;
    // What is wrong, and what to do, comes from the shared error report (the same text every panel shows).
    const auto report = reportFromReadiness (readiness);
    guidance.setText (report ? report->guidance : readiness.guidance, juce::dontSendNotification);
    details.setText (report ? report->details : readiness.details, false);
    recheckButton.setEnabled (! checking);
    detailsButton.setVisible (! checking && readiness.details.isNotEmpty());

    if (checking)
        startTimer (500);
    else
        stopTimer();

    if (checking || readiness.details.isEmpty())
        setDetailsShown (false);

    updateHeadline();
    resized();
    repaint();
}

int ReadinessPanel::getPreferredHeight() const
{
    return summaryHeight + (detailsShown ? detailsHeight + padding : 0);
}

void ReadinessPanel::paint (juce::Graphics& g)
{
    const auto background = findColour (juce::ResizableWindow::backgroundColourId).brighter (0.15f);
    g.setColour (background);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);

    const auto dot = juce::Rectangle<int> (padding, padding + 6, dotSize, dotSize).toFloat();
    g.setColour (statusColour());
    g.fillEllipse (dot);
}

void ReadinessPanel::resized()
{
    auto bounds = getLocalBounds().reduced (padding);
    auto summary = bounds.removeFromTop (summaryHeight - 2 * padding);

    auto buttons = summary.removeFromRight (230);
    recheckButton.setBounds (buttons.removeFromRight (100).withSizeKeepingCentre (100, 30));
    buttons.removeFromRight (10);
    detailsButton.setBounds (buttons.withSizeKeepingCentre (buttons.getWidth(), 30));

    summary.removeFromLeft (dotSize + 10);
    headline.setBounds (summary.removeFromTop (summary.getHeight() / 2 + 2));
    guidance.setBounds (summary);

    if (detailsShown)
    {
        bounds.removeFromTop (padding);
        details.setBounds (bounds);
    }
}

void ReadinessPanel::timerCallback()
{
    updateHeadline();
}

void ReadinessPanel::updateHeadline()
{
    if (current.state != Readiness::State::checking)
    {
        headline.setText (current.headline, juce::dontSendNotification);
        return;
    }

    const auto seconds = static_cast<int> ((juce::Time::getMillisecondCounterHiRes() - checkStartedMs) / 1000.0);
    headline.setText ("Checking the worker environment... " + juce::String (seconds) + " s",
                      juce::dontSendNotification);
}

void ReadinessPanel::setDetailsShown (bool shouldShow)
{
    if (detailsShown == shouldShow)
        return;

    detailsShown = shouldShow;
    details.setVisible (shouldShow);
    detailsButton.setButtonText (shouldShow ? "Hide details" : "Show details");

    if (onPreferredHeightChanged != nullptr)
        onPreferredHeightChanged();

    resized();
}

juce::Colour ReadinessPanel::statusColour() const
{
    switch (current.state)
    {
        case Readiness::State::checking:
            return juce::Colour (0xff8a8f98);
        case Readiness::State::ready:
            return juce::Colour (0xff3fb950);
        case Readiness::State::notSetUp:
        case Readiness::State::modelMissing:
            return juce::Colour (0xffd29922);
        case Readiness::State::error:
            return juce::Colour (0xfff85149);
    }

    return juce::Colour (0xfff85149);
}
} // namespace snds
