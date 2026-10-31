#include "ui/MixPanel.h"

namespace snds
{
MixPanel::MixPanel (MixService& mix)
    : mixService (mix),
      masterSection (mix.parameters()),
      tempoSection (mix.parameters())
{
    setName ("Mix");

    for (size_t i = 0; i < cards.size(); ++i)
    {
        cards[i] = std::make_unique<StemCard> (mix.parameters(), static_cast<dsp::Stem> (i));
        cards[i]->onPreferredHeightChanged = [this]
        {
            resized();

            if (onPreferredHeightChanged)
                onPreferredHeightChanged();
        };
        addAndMakeVisible (*cards[i]);
    }

    addAndMakeVisible (masterSection);
    addAndMakeVisible (tempoSection);

    noteLabel.setFont (juce::FontOptions (13.0f));
    noteLabel.setJustificationType (juce::Justification::centredRight);
    noteLabel.setMinimumHorizontalScale (0.8f);
    addAndMakeVisible (noteLabel);

    resetMixButton.setTitle ("Reset mix");
    resetMixButton.setTooltip ("Put every stem, effect and master control back to its default");
    resetMixButton.onClick = [this] { requestResetMix(); };
    addAndMakeVisible (resetMixButton);

    // Tab order: the four cards, the master, tempo and effects, then Reset mix.
    for (size_t i = 0; i < cards.size(); ++i)
        cards[i]->setExplicitFocusOrder (static_cast<int> (i) + 1);

    masterSection.setExplicitFocusOrder (5);
    tempoSection.setExplicitFocusOrder (6);
    resetMixButton.setExplicitFocusOrder (7);

    tempoSection.onTempoChosen = [this] (double bpm, const juce::String& mode) { applyTempo (bpm, mode); };
    tempoSection.onTempoCleared = [this]
    {
        if (onTempoCleared)
            onTempoCleared();
    };

    refresh();
    startTimerHz (30);
}

MixPanel::~MixPanel()
{
    stopTimer();
}

int MixPanel::getPreferredHeight() const noexcept
{
    int tallest = 0;

    for (const auto& card : cards)
        tallest = std::max (tallest, card->getPreferredHeight());

    return headerHeight + gap + tallest + gap + fxHeight;
}

juce::String MixPanel::describeNote (const PlaybackView& view)
{
    if (view.hasSong() && view.originalSelected)
        return "Original is playing, unprocessed. These controls shape Stem Mix.";

    return "These controls shape the stems. Original plays unprocessed, so its level can differ.";
}

void MixPanel::setSong (const std::optional<OpenSong>& newSong)
{
    song = newSong;
    haveSong = song.has_value();

    TempoFxSection::TempoInfo info;

    if (song)
    {
        info.detected = song->song().bpm.value;
        info.overridden = song->song().bpm.userOverride.has_value();
    }

    tempoSection.setTempoInfo (info);
    refresh();
}

double MixPanel::getResetTempo() const
{
    return song ? song->song().effectiveBpm() : static_cast<double> (dsp::tempoDefaultBpm);
}

juce::String MixPanel::describeReset (double bpm, const TempoFxSection::TempoInfo& info)
{
    const auto source = info.overridden ? juce::String ("the tempo you set")
                      : info.detected   ? juce::String ("the detected tempo")
                                        : juce::String ("the default tempo");
    auto bpmText = juce::String (bpm, 1).trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
    return "Every stem, effect and master control goes back to its default, and the tempo goes back to " + bpmText
         + " BPM (" + source + "). This cannot be undone.";
}

void MixPanel::resetMix()
{
    mixService.resetMix (getResetTempo());
    refresh(); // the store changed: bring every control back from it
}

void MixPanel::requestResetMix()
{
    TempoFxSection::TempoInfo info;

    if (song)
    {
        info.detected = song->song().bpm.value;
        info.overridden = song->song().bpm.userOverride.has_value();
    }

    const auto question = describeReset (getResetTempo(), info);
    auto proceed = [safe = juce::Component::SafePointer<MixPanel> (this)]
    {
        if (safe != nullptr)
            safe->resetMix();
    };

    if (confirmReset)
    {
        confirmReset (question, std::move (proceed));
        return;
    }

    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon,
                                        "Reset the mix?",
                                        question,
                                        "Reset",
                                        "Cancel",
                                        this,
                                        juce::ModalCallbackFunction::create (
                                            [proceed] (int result)
                                            {
                                                if (result == 1)
                                                    proceed();
                                            }));
}

void MixPanel::applyTempo (double bpm, const juce::String& mode)
{
    mixService.parameters().set (dsp::GlobalControl::tempoBpm, static_cast<float> (bpm));
    tempoSection.syncFromStore();

    if (onTempoChosen)
        onTempoChosen (bpm, mode);
}

void MixPanel::refresh()
{
    PlaybackView view;

    if (playback != nullptr)
        view = playback->view();

    // Without a player to ask, a loaded song is one that has a processor.
    const bool songLoaded = playback != nullptr ? view.hasSong() : mixService.processor() != nullptr;

    if (songLoaded != isEnabled())
        setEnabled (songLoaded);

    // Controls follow the store (a reset, a new song), not the other way round.
    const auto version = mixService.parameters().version();

    if (version != lastVersion)
    {
        lastVersion = version;

        for (auto& card : cards)
            card->syncFromStore();

        masterSection.syncFromStore();
        tempoSection.syncFromStore();
    }

    dsp::MeterSnapshot snapshot;

    if (songLoaded && mixService.processor() != nullptr)
    {
        if (mixService.readMeters (snapshot))
        {
            for (size_t i = 0; i < cards.size(); ++i)
                cards[i]->setLevels (snapshot.stems[i]);

            masterSection.update (snapshot, juce::Time::getMillisecondCounterHiRes());
        } // a collided read keeps the last values
    }
    else
    {
        for (auto& card : cards)
            card->clearLevels();

        masterSection.clearLevels();
    }

    const auto note = describeNote (view);

    if (noteLabel.getText() != note)
        noteLabel.setText (note, juce::dontSendNotification);

    noteLabel.setColour (juce::Label::textColourId,
                         view.originalSelected ? juce::Colour (0xffd29922)
                                               : findColour (juce::Label::textColourId).withAlpha (0.65f));
}

void MixPanel::paint (juce::Graphics& g)
{
    g.setColour (findColour (juce::Label::textColourId));
    g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    g.drawText (
        "Mix", getLocalBounds().removeFromTop (headerHeight).removeFromLeft (80), juce::Justification::centredLeft);
}

void MixPanel::resized()
{
    auto area = getLocalBounds();
    auto header = area.removeFromTop (headerHeight);
    resetMixButton.setBounds (header.withTrimmedLeft (70).removeFromLeft (100).reduced (0, 2));
    noteLabel.setBounds (header.withTrimmedLeft (180));
    area.removeFromTop (gap);

    auto fx = area.removeFromBottom (fxHeight);
    area.removeFromBottom (gap);
    tempoSection.setBounds (fx);

    masterSection.setBounds (area.removeFromRight (masterWidth).withHeight (StemCard::collapsedHeight));
    area.removeFromRight (gap);

    const auto cardWidth = (area.getWidth() - 3 * gap) / 4;

    for (auto& card : cards)
    {
        card->setBounds (area.removeFromLeft (cardWidth).withHeight (card->getPreferredHeight()));
        area.removeFromLeft (gap);
    }
}
} // namespace snds
