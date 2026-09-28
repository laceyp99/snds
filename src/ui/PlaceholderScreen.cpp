#include "ui/PlaceholderScreen.h"

namespace snds::ui
{

PlaceholderScreen::PlaceholderScreen (const juce::String& title, const juce::String& body)
{
    titleLabel.setText (title, juce::dontSendNotification);
    titleLabel.setFont (juce::FontOptions (26.0f, juce::Font::bold));
    titleLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (titleLabel);

    bodyLabel.setText (body, juce::dontSendNotification);
    bodyLabel.setFont (juce::FontOptions (16.0f));
    bodyLabel.setJustificationType (juce::Justification::topLeft);
    bodyLabel.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (bodyLabel);
}

void PlaceholderScreen::addNavigation (const juce::String& label, std::function<void()> onClick)
{
    auto button = std::make_unique<juce::TextButton> (label);
    button->onClick = std::move (onClick);
    addAndMakeVisible (*button);
    buttons.push_back (std::move (button));
    resized();
}

void PlaceholderScreen::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void PlaceholderScreen::resized()
{
    auto area = getLocalBounds().reduced (32);

    titleLabel.setBounds (area.removeFromTop (44));
    area.removeFromTop (12);

    auto buttonRow = area.removeFromBottom (36);
    area.removeFromBottom (16);
    bodyLabel.setBounds (area);

    for (auto& button : buttons)
    {
        button->setBounds (buttonRow.removeFromLeft (200));
        buttonRow.removeFromLeft (12);
    }
}

} // namespace snds::ui
