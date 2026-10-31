#include "ui/MasterSection.h"

namespace snds
{
namespace
{
constexpr int pad = 10;
const juce::Colour limiterColour (0xffe5534b);
} // namespace

MasterSection::MasterSection (dsp::ParameterStore& parameterStore)
    : gain (parameterStore, dsp::parameterIndex (dsp::GlobalControl::masterGain), juce::Slider::LinearVertical)
{
    setName ("Master");
    title.setText ("Master", juce::dontSendNotification);
    title.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    addAndMakeVisible (title);

    gain.setTitle ("Master gain");
    addAndMakeVisible (gain);
    addAndMakeVisible (meterBar);
}

juce::String MasterSection::limiterText (const dsp::MeterSnapshot& snapshot)
{
    return snapshot.limiterActive() ? "Limiter -" + juce::String (snapshot.limiterReductionDb, 1) + " dB"
                                    : juce::String ("Limiter idle");
}

void MasterSection::update (const dsp::MeterSnapshot& snapshot, double nowMs)
{
    meterBar.setLevels (snapshot.masterPeak, snapshot.masterRms);

    const bool active = snapshot.limiterActive();

    if (active && ! lit)
        litSince = nowMs;

    if (! active)
        litSince = -1.0;

    const bool advise = active && litSince >= 0.0 && nowMs - litSince >= adviceAfterMs;
    const auto text = limiterText (snapshot);

    if (active != lit || advise != adviceShown || text != lampText)
    {
        lit = active;
        adviceShown = advise;
        lampText = text;
        repaint();
    }
}

void MasterSection::clearLevels()
{
    meterBar.clear();

    if (lit || adviceShown)
    {
        lit = false;
        adviceShown = false;
        litSince = -1.0;
        lampText = "Limiter idle";
        repaint();
    }
}

void MasterSection::paint (juce::Graphics& g)
{
    g.setColour (findColour (juce::ResizableWindow::backgroundColourId).brighter (0.1f));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
    g.setColour (juce::Colours::white.withAlpha (0.5f));
    g.fillRoundedRectangle (juce::Rectangle<float> (0.0f, 0.0f, static_cast<float> (getWidth()), 3.0f), 1.5f);

    const auto lamp = lampArea.toFloat();
    g.setColour (lit ? limiterColour : juce::Colours::black.withAlpha (0.4f));
    g.fillRoundedRectangle (lamp, 4.0f);
    g.setColour (lit ? juce::Colours::white : findColour (juce::Label::textColourId).withAlpha (0.7f));
    g.setFont (juce::FontOptions (13.0f, lit ? juce::Font::bold : juce::Font::plain));
    g.drawText (lampText, lampArea, juce::Justification::centred);

    if (adviceShown)
    {
        g.setColour (juce::Colour (0xffd29922));
        g.setFont (juce::FontOptions (12.0f));
        g.drawFittedText (
            "Too hot. Lower the master gain, stem gains or effect sends.", adviceArea, juce::Justification::topLeft, 4);
    }
}

void MasterSection::resized()
{
    auto area = getLocalBounds().reduced (pad);
    title.setBounds (area.removeFromTop (22));
    area.removeFromTop (4);

    auto faderArea = area.removeFromTop (136);
    gain.setBounds (faderArea.removeFromLeft (faderArea.getWidth() - 26));
    faderArea.removeFromLeft (6);
    meterBar.setBounds (faderArea.withTrimmedBottom (22));

    area.removeFromTop (6);
    lampArea = area.removeFromTop (26);
    area.removeFromTop (4);
    adviceArea = area;
}
} // namespace snds
