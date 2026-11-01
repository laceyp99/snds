#include "ui/FullscreenVisuals.h"

#include <algorithm>
#include <cmath>

namespace snds
{
namespace
{
juce::String clock (double seconds)
{
    const auto total = static_cast<int> (std::round (juce::jmax (0.0, seconds)));
    return juce::String (total / 60) + ":" + juce::String (total % 60).paddedLeft ('0', 2);
}
} // namespace

FullscreenVisualsContent::FullscreenVisualsContent (rendering::VisualHost& visualHost,
                                                    rendering::VisualController& controller,
                                                    FullscreenActions a,
                                                    juce::String songTitle)
    : host (&visualHost),
      visuals (controller),
      actions (std::move (a)),
      title (std::move (songTitle))
{
    setOpaque (true);
    setWantsKeyboardFocus (true);

    titleLabel.setText (title, juce::dontSendNotification);
    titleLabel.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    titleLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible (titleLabel);

    statusLabel.setFont (juce::FontOptions (12.0f));
    statusLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.65f));
    statusLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (statusLabel);

    exitButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffb3261e));
    exitButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    exitButton.setTooltip ("Leave fullscreen (Esc)");
    exitButton.onClick = [this] { requestExit(); };
    addAndMakeVisible (exitButton);

    playButton.onClick = [this]
    {
        if (actions.togglePlay)
            actions.togglePlay();
    };
    addAndMakeVisible (playButton);

    position.setRange (0.0, 1.0, 0.0);
    position.setSliderStyle (juce::Slider::LinearHorizontal);
    position.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    position.onDragStart = [this] { dragging = true; };
    position.onDragEnd = [this]
    {
        dragging = false;

        if (actions.seekToFraction)
            actions.seekToFraction (position.getValue());
    };
    addAndMakeVisible (position);

    timeLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    timeLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (timeLabel);

    effectsToggle.setToggleState (visuals.getSettings().effectsEnabled, juce::dontSendNotification);
    effectsToggle.onClick = [this] { visuals.setEffectsEnabled (effectsToggle.getToggleState()); };
    addAndMakeVisible (effectsToggle);

    intensity.setRange (0.0, 100.0, 1.0);
    intensity.setSliderStyle (juce::Slider::LinearHorizontal);
    intensity.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 22);
    intensity.setTextValueSuffix ("%");
    intensity.setValue (static_cast<double> (visuals.getSettings().intensity) * 100.0, juce::dontSendNotification);
    intensity.onValueChange = [this] { visuals.setIntensity (static_cast<float> (intensity.getValue() / 100.0)); };
    addAndMakeVisible (intensity);

    addAndMakeVisible (*host); // takes the host from the player's panel

    startTimerHz (20);
    refresh();
}

FullscreenVisualsContent::~FullscreenVisualsContent()
{
    stopTimer();
    releaseHost();
}

void FullscreenVisualsContent::releaseHost()
{
    if (host != nullptr)
    {
        removeChildComponent (host);
        host = nullptr;
    }
}

juce::Colour FullscreenVisualsContent::barColourFor (const rendering::ThemePalette& palette)
{
    const auto base = defaultBarColour();

    if (! palette.fromArtwork)
        return base;

    for (const auto amount : { 0.10f, 0.05f, 0.02f })
    {
        const auto tinted = base.interpolatedWith (palette.accent, amount);

        if (rendering::contrastRatio (juce::Colours::white, tinted) >= rendering::ThemePalette::bodyTextContrast)
            return tinted;
    }

    return base;
}

void FullscreenVisualsContent::setTheme (const rendering::ThemePalette& palette)
{
    barColour = barColourFor (palette);

    auto style = [&palette] (juce::Component& c, int id, juce::Colour colour)
    {
        if (palette.fromArtwork)
            c.setColour (id, colour);
        else
            c.removeColour (id);
    };

    // The accent is checked against the panel colour; here it sits on the bar, so check it again.
    const auto accent = rendering::ensureContrast (palette.accent, barColour, rendering::ThemePalette::textContrast);
    const auto onAccent = rendering::contrastRatio (juce::Colours::white, accent)
                               >= rendering::contrastRatio (juce::Colours::black, accent)
                            ? juce::Colours::white
                            : juce::Colours::black;

    style (position, juce::Slider::thumbColourId, accent);
    style (position, juce::Slider::trackColourId, accent);
    style (intensity, juce::Slider::thumbColourId, accent);
    style (intensity, juce::Slider::trackColourId, accent);
    style (effectsToggle, juce::ToggleButton::tickColourId, accent);
    style (playButton, juce::TextButton::buttonColourId, accent);
    style (playButton, juce::TextButton::textColourOffId, onAccent);
    repaint();
}

void FullscreenVisualsContent::setStatusText (const juce::String& text)
{
    statusLabel.setText (text, juce::dontSendNotification);
}

void FullscreenVisualsContent::requestExit()
{
    if (onExit)
        onExit();
}

void FullscreenVisualsContent::parentHierarchyChanged()
{
    if (isShowing())
        grabKeyboardFocus();
}

void FullscreenVisualsContent::resized()
{
    auto area = getLocalBounds();

    auto top = area.removeFromTop (topBarHeight).reduced (12, 8);
    exitButton.setBounds (top.removeFromRight (190));
    top.removeFromRight (12);
    statusLabel.setBounds (top.removeFromRight (juce::jmin (480, top.getWidth() / 2)));
    titleLabel.setBounds (top);

    auto bottom = area.removeFromBottom (bottomBarHeight).reduced (12, 14);
    playButton.setBounds (bottom.removeFromLeft (84));
    bottom.removeFromLeft (10);
    intensity.setBounds (bottom.removeFromRight (190));
    bottom.removeFromRight (6);
    effectsToggle.setBounds (bottom.removeFromRight (84));
    bottom.removeFromRight (6);
    timeLabel.setBounds (bottom.removeFromRight (104));
    position.setBounds (bottom);

    visualsArea = area;

    if (host != nullptr)
        host->setBounds (visualsArea);
}

void FullscreenVisualsContent::paint (juce::Graphics& g)
{
    g.fillAll (defaultBarColour());
    g.setColour (barColour);
    g.fillRect (getLocalBounds().removeFromTop (topBarHeight));
    g.fillRect (getLocalBounds().removeFromBottom (bottomBarHeight));
}

bool FullscreenVisualsContent::keyPressed (const juce::KeyPress& key)
{
    if (key.isKeyCode (juce::KeyPress::escapeKey))
    {
        requestExit();
        return true;
    }

    if (key.isKeyCode (juce::KeyPress::spaceKey))
    {
        if (actions.togglePlay)
            actions.togglePlay();
        return true;
    }

    const auto step = [this] (double seconds)
    {
        if (! actions.view || ! actions.seekToFraction)
            return;

        const auto view = actions.view();

        if (view.durationSeconds > 0.0)
            actions.seekToFraction (juce::jlimit (0.0, 1.0, view.fraction + seconds / view.durationSeconds));
    };

    if (key.isKeyCode (juce::KeyPress::leftKey))
    {
        step (-seekStepSeconds);
        return true;
    }

    if (key.isKeyCode (juce::KeyPress::rightKey))
    {
        step (seekStepSeconds);
        return true;
    }

    if (key.isKeyCode (juce::KeyPress::homeKey))
    {
        if (actions.seekToFraction)
            actions.seekToFraction (0.0);
        return true;
    }

    return false;
}

void FullscreenVisualsContent::refresh()
{
    if (! actions.view)
        return;

    const auto view = actions.view();
    playButton.setButtonText (view.state == PlaybackState::playing ? "Pause" : "Play");
    playButton.setEnabled (view.canPlay());
    position.setEnabled (view.canSeek());
    timeLabel.setText (clock (view.positionSeconds) + " / " + clock (view.durationSeconds), juce::dontSendNotification);

    if (! dragging)
    {
        const juce::ScopedValueSetter<bool> guard (updating, true);
        position.setValue (view.fraction, juce::dontSendNotification);
    }
}

//==============================================================================
FullscreenVisualsWindow::FullscreenVisualsWindow (std::unique_ptr<FullscreenVisualsContent> content,
                                                  juce::Component* nearComponent,
                                                  bool kioskMode)
    : DocumentWindow ("SNDS visuals", juce::Colours::black, 0)
{
    setUsingNativeTitleBar (false);
    setTitleBarHeight (0);
    setResizable (false, false);
    setContentOwned (content.release(), false);
    setDropShadowEnabled (false);

    // The display the main window is on; the kiosk mode then covers all of it, taskbar included.
    const auto& displays = juce::Desktop::getInstance().getDisplays();
    const auto area = nearComponent != nullptr
                        ? displays.getDisplayForRect (nearComponent->getScreenBounds())->logicalBounds
                        : displays.getPrimaryDisplay()->logicalBounds;
    // Kiosk mode covers the whole display; without it (tests) an ordinary window of a modest size.
    setBounds (kioskMode ? area.toNearestInt() : area.toNearestInt().withSizeKeepingCentre (900, 560));
    setVisible (true);

    if (kioskMode)
        juce::Desktop::getInstance().setKioskModeComponent (this, false);
    getContent().grabKeyboardFocus();
}

FullscreenVisualsWindow::~FullscreenVisualsWindow()
{
    if (juce::Desktop::getInstance().getKioskModeComponent() == this)
        juce::Desktop::getInstance().setKioskModeComponent (nullptr, false);

    getContent().releaseHost(); // the player takes its visuals back before the screen goes
}
} // namespace snds
