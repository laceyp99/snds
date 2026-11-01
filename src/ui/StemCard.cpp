#include "ui/StemCard.h"

namespace snds
{
namespace
{
constexpr std::array<dsp::StemControl, 5> secondaryControls { dsp::StemControl::pan,
                                                              dsp::StemControl::lowCut,
                                                              dsp::StemControl::highCut,
                                                              dsp::StemControl::reverbSend,
                                                              dsp::StemControl::delaySend };

constexpr std::array<const char*, 5> secondaryNames { "Pan", "Low cut", "High cut", "Reverb", "Delay" };

constexpr int pad = 10;
constexpr int rowHeight = 28;
} // namespace

juce::Colour stemAccent (dsp::Stem stem)
{
    switch (stem)
    {
        case dsp::Stem::drums:
            return juce::Colour (0xffe8833a);
        case dsp::Stem::bass:
            return juce::Colour (0xff3d8ee0);
        case dsp::Stem::other:
            return juce::Colour (0xff58b368);
        case dsp::Stem::vocals:
            return juce::Colour (0xffd45f9a);
    }

    return juce::Colours::grey;
}

StemCard::StemCard (dsp::ParameterStore& parameterStore, dsp::Stem whichStem)
    : store (parameterStore),
      stem (whichStem),
      accent (stemAccent (whichStem)),
      gain (parameterStore, dsp::parameterIndex (whichStem, dsp::StemControl::gain), juce::Slider::LinearVertical),
      mute (parameterStore, dsp::parameterIndex (whichStem, dsp::StemControl::mute), "M"),
      solo (parameterStore, dsp::parameterIndex (whichStem, dsp::StemControl::solo), "S")
{
    const auto name = juce::String (dsp::stemNames[static_cast<size_t> (stem)].data(),
                                    dsp::stemNames[static_cast<size_t> (stem)].size());
    setName (name);
    title.setText (name, juce::dontSendNotification);
    title.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    title.setColour (juce::Label::textColourId, accent);
    addAndMakeVisible (title);

    gain.setColour (juce::Slider::trackColourId, accent.withAlpha (0.7f));
    gain.setColour (juce::Slider::thumbColourId, accent);
    gain.setTitle (name + " gain");
    addAndMakeVisible (gain);

    meterBar.setAccent (accent);
    addAndMakeVisible (meterBar);

    mute.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffe5534b));
    solo.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd8b232));
    mute.setTitle (name + " mute");
    solo.setTitle (name + " solo");
    mute.setTooltip ("Mute " + name);
    solo.setTooltip ("Solo " + name + " (additive: soloing several plays them together)");
    addAndMakeVisible (mute);
    addAndMakeVisible (solo);

    moreToggle.setClickingTogglesState (true);
    moreToggle.setTooltip ("Pan, low cut, high cut, reverb send and delay send");
    moreToggle.onClick = [this] { setExpanded (moreToggle.getToggleState()); };
    addAndMakeVisible (moreToggle);
    moreToggle.setTitle (name + " more controls");

    // Tab order inside the card: gain, mute, solo, More, then the expanded controls.
    gain.setExplicitFocusOrder (1);
    mute.setExplicitFocusOrder (2);
    solo.setExplicitFocusOrder (3);
    moreToggle.setExplicitFocusOrder (4);

    for (size_t i = 0; i < secondaryCount; ++i)
    {
        secondary[i] = std::make_unique<ParameterSlider> (
            parameterStore, dsp::parameterIndex (stem, secondaryControls[i]), juce::Slider::LinearHorizontal);
        secondary[i]->setColour (juce::Slider::trackColourId, accent.withAlpha (0.55f));
        secondary[i]->setColour (juce::Slider::thumbColourId, accent);
        secondary[i]->setTitle (name + " " + secondaryNames[i]);
        secondary[i]->setExplicitFocusOrder (5 + static_cast<int> (i));
        secondaryLabels[i].setText (secondaryNames[i], juce::dontSendNotification);
        secondaryLabels[i].setFont (juce::FontOptions (13.0f));
        addChildComponent (*secondary[i]);
        addChildComponent (secondaryLabels[i]);
    }

    setWantsKeyboardFocus (false);
}

ParameterSlider& StemCard::secondarySlider (dsp::StemControl control)
{
    for (size_t i = 0; i < secondaryCount; ++i)
        if (secondaryControls[i] == control)
            return *secondary[i];

    jassertfalse;
    return *secondary[0];
}

void StemCard::syncFromStore()
{
    gain.syncFromStore();
    mute.syncFromStore();
    solo.syncFromStore();

    for (auto& slider : secondary)
        slider->syncFromStore();
}

void StemCard::setExpanded (bool shouldExpand)
{
    if (expanded == shouldExpand)
        return;

    expanded = shouldExpand;
    moreToggle.setToggleState (expanded, juce::dontSendNotification);
    moreToggle.setButtonText (expanded ? "Less" : "More");

    for (size_t i = 0; i < secondaryCount; ++i)
    {
        secondary[i]->setVisible (expanded);
        secondaryLabels[i].setVisible (expanded);
    }

    resized();

    if (onPreferredHeightChanged)
        onPreferredHeightChanged();
}

void StemCard::paint (juce::Graphics& g)
{
    g.setColour (findColour (juce::ResizableWindow::backgroundColourId).brighter (0.1f));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
    g.setColour (accent.withAlpha (0.8f));
    g.fillRoundedRectangle (juce::Rectangle<float> (0.0f, 0.0f, static_cast<float> (getWidth()), 3.0f), 1.5f);
}

void StemCard::resized()
{
    auto top = getLocalBounds().reduced (pad);
    title.setBounds (top.removeFromTop (22));
    top.removeFromTop (4);
    auto faderArea = top.removeFromTop (136);
    gain.setBounds (faderArea.removeFromLeft (faderArea.getWidth() - 26));
    faderArea.removeFromLeft (6);
    meterBar.setBounds (faderArea.withTrimmedBottom (22));

    top.removeFromTop (6);
    auto buttonRow = top.removeFromTop (26);
    const auto half = (buttonRow.getWidth() - 6) / 2;
    mute.setBounds (buttonRow.removeFromLeft (half));
    buttonRow.removeFromLeft (6);
    solo.setBounds (buttonRow);

    top.removeFromTop (6);
    moreToggle.setBounds (top.removeFromTop (26));

    if (expanded)
    {
        top.removeFromTop (8);

        for (size_t i = 0; i < secondaryCount; ++i)
        {
            auto row = top.removeFromTop (rowHeight);
            secondaryLabels[i].setBounds (row.removeFromTop (14));
            secondary[i]->setBounds (row);
        }
    }
}
} // namespace snds
