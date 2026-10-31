#include "ui/SongPanel.h"

#include "model/Artwork.h"
#include "rendering/VisualStatus.h"

#include <cmath>

namespace snds
{
namespace
{
constexpr int padding = 16;
constexpr int detailHeight = 44;
constexpr int controlsHeight = 28;
constexpr int visualsTickHz = 30;
} // namespace

juce::String formatDuration (double seconds)
{
    const auto total = static_cast<int> (std::round (juce::jmax (0.0, seconds)));
    return juce::String (total / 60) + ":" + juce::String (total % 60).paddedLeft ('0', 2);
}

SongPanel::SongPanel (rendering::RenderOverrides overrides, rendering::VisualHost::RendererFactory factory)
    : visualHost (std::move (overrides), std::move (factory))
{
    locateButton.onClick = [this]
    {
        if (onLocateOriginal)
            onLocateOriginal();
    };
    addChildComponent (locateButton);
    addAndMakeVisible (transport);

    addChildComponent (visualHost); // shown with the first song; started once the panel is on screen

    effectsToggle.setToggleState (visuals.getSettings().effectsEnabled, juce::dontSendNotification);
    effectsToggle.setTooltip ("Turn the artwork effects on or off");
    effectsToggle.onClick = [this] { visuals.setEffectsEnabled (effectsToggle.getToggleState()); };
    addAndMakeVisible (effectsToggle);

    intensityLabel.setText ("Intensity", juce::dontSendNotification);
    intensityLabel.setFont (juce::FontOptions (13.0f));
    addAndMakeVisible (intensityLabel);

    intensitySlider.setRange (0.0, 100.0, 1.0);
    intensitySlider.setSliderStyle (juce::Slider::LinearHorizontal);
    intensitySlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 22);
    intensitySlider.setTextValueSuffix ("%");
    intensitySlider.setTooltip ("How strongly the artwork reacts to the music");
    intensitySlider.setValue (static_cast<double> (visuals.getSettings().intensity) * 100.0,
                              juce::dontSendNotification);
    intensitySlider.onValueChange = [this]
    { visuals.setIntensity (static_cast<float> (intensitySlider.getValue() / 100.0)); };
    addAndMakeVisible (intensitySlider);

    fullscreenButton.setTooltip ("Show the visuals on the whole screen (Esc leaves)");
    fullscreenButton.onClick = [this] { enterFullscreen(); };
    fullscreenButton.setEnabled (false);
    addAndMakeVisible (fullscreenButton);

    detailLabel.setFont (juce::FontOptions (11.0f));
    detailLabel.setJustificationType (juce::Justification::topLeft);
    detailLabel.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (detailLabel);

    visualHost.setFallbackListener (
        [this] (const rendering::FallbackEvent& event)
        {
            if (event.cause == rendering::FallbackCause::unavailable || ! onRendererFallback)
                return;

            onRendererFallback ({ rendering::tierName (event.from),
                                  rendering::tierName (event.to),
                                  rendering::causeName (event.cause),
                                  event.detail });
        });

    canStartVisuals = [this] { return isShowing() && getPeer() != nullptr; };
    startTimerHz (visualsTickHz);
}

SongPanel::~SongPanel()
{
    stopTimer();
    exitFullscreen();
}

void SongPanel::setLocateEnabled (bool enabled)
{
    locateButton.setEnabled (enabled);
}

void SongPanel::setSong (const std::optional<OpenSong>& newSong)
{
    song = newSong;
    artwork = {};

    if (song)
    {
        const auto file = song->artworkFile();

        if (file.existsAsFile())
            artwork = juce::ImageFileFormat::loadFrom (file);

        if (! artwork.isValid())
            artwork = makeFallbackArtwork (song->song().source.fileName, artworkSize * 2);
    }

    refreshTheme();

    rendering::ArtworkInput input;
    input.image = artwork;
    input.palette = rendering::derivePalette (artwork);
    visualHost.setArtwork (input);
    visuals.reset(); // a new song starts from the plain artwork

    visualHost.setVisible (song.has_value() || visualHost.isStarted());
    fullscreenButton.setEnabled (song.has_value());

    if (! song)
        exitFullscreen();

    locateButton.setVisible (song.has_value() && ! song->originalAvailable);
    resized();
    repaint();
    updateDetail();
}

//==============================================================================
juce::Colour SongPanel::basePanelColour() const
{
    return findColour (juce::ResizableWindow::backgroundColourId).brighter (0.15f);
}

void SongPanel::refreshTheme()
{
    const auto panel = basePanelColour();
    const auto text = findColour (juce::Label::textColourId);
    theme = song.has_value() ? rendering::makeThemePalette (artwork, panel, text)
                             : rendering::defaultThemePalette (panel, text);

    if (song.has_value() && ! theme.fromArtwork)
        juce::Logger::writeToLog ("visuals: using the default colours because " + theme.reason);

    applyTheme();
}

void SongPanel::applyTheme()
{
    auto style = [this] (juce::Component& c, int id, juce::Colour colour)
    {
        if (theme.fromArtwork)
            c.setColour (id, colour);
        else
            c.removeColour (id); // back to the look and feel's own colour
    };

    style (intensitySlider, juce::Slider::thumbColourId, theme.accent);
    style (intensitySlider, juce::Slider::trackColourId, theme.accent);
    style (effectsToggle, juce::ToggleButton::tickColourId, theme.accent);
    style (fullscreenButton, juce::TextButton::buttonColourId, theme.accent);
    style (fullscreenButton, juce::TextButton::textColourOffId, theme.onAccent);

    if (fullscreen != nullptr)
        fullscreen->getContent().setTheme (theme);

    repaint();
}

void SongPanel::lookAndFeelChanged()
{
    refreshTheme();
}

bool SongPanel::isHostFullyInView() const
{
    // Walk up the parents, clipping to each (a scrolled-out or half-covered host is not fully in view).
    auto visible = visualHost.getLocalBounds();
    const juce::Component* current = &visualHost;

    while (const auto* parent = current->getParentComponent())
    {
        visible = parent->getLocalArea (current, visible).getIntersection (parent->getLocalBounds());

        if (visible.isEmpty())
            return false;

        current = parent;
    }

    return visible == current->getLocalArea (&visualHost, visualHost.getLocalBounds());
}

void SongPanel::syncVisualControls()
{
    effectsToggle.setToggleState (visuals.getSettings().effectsEnabled, juce::dontSendNotification);
    intensitySlider.setValue (static_cast<double> (visuals.getSettings().intensity) * 100.0,
                              juce::dontSendNotification);
}

void SongPanel::updateDetail()
{
    detailText = rendering::describeVisuals (visualHost.tier(),
                                             visualHost.status(),
                                             visualHost.events(),
                                             visuals.getSettings().staticMode,
                                             visuals.getSettings().effectsEnabled);
    detailLabel.setText (detailText, juce::dontSendNotification);

    if (fullscreen != nullptr)
        fullscreen->getContent().setStatusText (detailText);

    ticksSinceDetail = 0;
}

void SongPanel::tickVisuals()
{
    if (song && ! visualHost.isStarted() && canStartVisuals && canStartVisuals())
    {
        visualHost.setVisible (true);
        visualHost.start();
        updateDetail();
    }

    if (fullscreen == nullptr)
        visualHost.setOccluded (! isHostFullyInView());

    // What the user has chosen on the transport, and whether music is actually playing.
    const auto view = viewSource ? viewSource() : PlaybackView();
    visuals.setOriginalSelected (view.originalSelected);

    dsp::VisualSnapshot snapshot; // silence: the picture settles when nothing plays
    const auto playing = view.state == PlaybackState::playing && ! view.buffering;

    if (playing && mixService != nullptr)
        if (const auto processor = mixService->processor())
            processor->visuals().read (snapshot);

    // A still picture has nothing to animate: skip the work (the Original, or the static tier).
    if (visualHost.tier() != rendering::RenderTier::staticImage)
        visuals.update (snapshot, clockMs());

    if (++ticksSinceDetail >= visualsTickHz / 2)
        updateDetail();
}

//==============================================================================
void SongPanel::enterFullscreen()
{
    if (fullscreen != nullptr || ! song)
        return;

    FullscreenActions actions;
    actions.view = [this] { return viewSource ? viewSource() : PlaybackView(); };
    actions.togglePlay = [this]
    {
        if (playback != nullptr)
            playback->togglePlay();
    };
    actions.seekToFraction = [this] (double fraction)
    {
        if (playback != nullptr)
            playback->seekToFraction (fraction);
    };

    auto content = std::make_unique<FullscreenVisualsContent> (
        visualHost, visuals, std::move (actions), song->song().source.fileName);

    // Leaving is deferred: it must not destroy the screen from inside its own key or button handler.
    const auto leave = [safe = juce::Component::SafePointer<SongPanel> (this)]
    {
        juce::MessageManager::callAsync (
            [safe]
            {
                if (safe != nullptr)
                    safe->exitFullscreen();
            });
    };
    content->onExit = leave;
    content->setStatusText (detailText);

    content->setTheme (theme);
    fullscreen = std::make_unique<FullscreenVisualsWindow> (std::move (content), this, useKioskMode);
    fullscreen->onExit = leave;
}

void SongPanel::exitFullscreen()
{
    if (fullscreen == nullptr)
        return;

    fullscreen.reset(); // hands the visuals back (the content releases the host)
    addAndMakeVisible (visualHost);
    resized();
    syncVisualControls(); // the screen has its own controls for the same two settings
    repaint();
}

//==============================================================================
void SongPanel::paint (juce::Graphics& g)
{
    const auto text = findColour (juce::Label::textColourId);
    g.setColour (theme.fromArtwork ? theme.panelBackground : basePanelColour());
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);

    auto content = getLocalBounds().reduced (padding);
    content.removeFromBottom (TransportBar::preferredHeight + 8);

    if (! song)
    {
        g.setColour (text);
        g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
        g.drawText ("Player", content.removeFromTop (28), juce::Justification::centredLeft);
        g.setColour (text.withAlpha (0.7f));
        g.setFont (juce::FontOptions (15.0f));
        g.drawText ("No song loaded.", content.removeFromTop (24), juce::Justification::centredLeft);
        return;
    }

    const auto& metadata = song->song();
    content.removeFromLeft (artworkSize + padding); // the visuals (a child component) and their detail line

    g.setColour (text);
    g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    const auto titleArea = content.removeFromTop (28);
    g.drawText (metadata.source.fileName, titleArea, juce::Justification::centredLeft, true);

    if (theme.fromArtwork) // a short accent rule under the title, in the artwork's own colour
    {
        g.setColour (theme.accent);
        g.fillRect (titleArea.getX(), titleArea.getBottom(), 56, 2);
        g.setColour (text);
    }

    g.setColour (text.withAlpha (0.8f));
    g.setFont (juce::FontOptions (14.0f));

    const auto tempo = song->bpmIsDefault() ? juce::String ("Tempo: not detected, using 120 BPM until you set one")
                                            : "Tempo: " + juce::String (song->song().effectiveBpm(), 1) + " BPM";
    const auto device = metadata.worker.device == "cuda" ? "GPU" : "CPU";

    for (const auto& line :
         { formatDuration (metadata.source.durationSeconds) + "  -  " + juce::String (metadata.source.sampleRate)
               + " Hz  -  " + (metadata.source.channels == 1 ? "mono" : "stereo") + "  -  "
               + metadata.source.container.toUpperCase(),
           tempo,
           "Stems ready: drums, bass, other, vocals (prepared on the " + juce::String (device) + ")" })
        g.drawText (line, content.removeFromTop (22), juce::Justification::centredLeft, true);

    content.removeFromTop (6);

    if (song->originalAvailable)
    {
        g.drawText ("Original: " + song->originalFile.getFullPathName(),
                    content.removeFromTop (22),
                    juce::Justification::centredLeft,
                    true);
    }
    else
    {
        g.setColour (juce::Colour (0xffd29922));
        g.drawFittedText (
            "Original unavailable. " + song->originalNote, content.removeFromTop (44), juce::Justification::topLeft, 2);
    }
}

void SongPanel::resized()
{
    auto content = getLocalBounds().reduced (padding);
    transport.setBounds (content.removeFromBottom (TransportBar::preferredHeight));
    content.removeFromBottom (8);

    // Left column: the visuals, and under them the renderer detail line.
    auto left = content.removeFromLeft (artworkSize);

    if (fullscreen == nullptr)
        visualHost.setBounds (left.removeFromTop (artworkSize));
    else
        left.removeFromTop (artworkSize);

    left.removeFromTop (4);
    detailLabel.setBounds (left.removeFromTop (detailHeight));
    content.removeFromLeft (padding);

    // Right column, from the bottom: the visual controls, then Locate Original.
    auto controls = content.removeFromBottom (controlsHeight);
    effectsToggle.setBounds (controls.removeFromLeft (80));
    intensityLabel.setBounds (controls.removeFromLeft (62));
    fullscreenButton.setBounds (controls.removeFromRight (100));
    controls.removeFromRight (8);
    intensitySlider.setBounds (controls.reduced (0, 2));

    content.removeFromBottom (4);
    locateButton.setBounds (content.removeFromBottom (32).removeFromLeft (190));
}
} // namespace snds
