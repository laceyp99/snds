#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "app/PlaybackController.h"
#include "rendering/ArtworkPalette.h"
#include "rendering/VisualController.h"
#include "rendering/VisualHost.h"

#include <functional>
#include <memory>

namespace snds
{
/** What the fullscreen controls need from the player, as plain callbacks so the screen does not
    depend on PlaybackController (and tests can fake it). All are called on the message thread. */
struct FullscreenActions
{
    std::function<PlaybackView()> view;
    std::function<void()> togglePlay;
    std::function<void (double fraction)> seekToFraction;
};

/** The fullscreen visuals screen: the artwork visuals fill the middle, with a slim top bar (the song
    and a prominent "Exit fullscreen" button) and a slim bottom bar (play/pause, position, time,
    effects on/off and intensity). The mix controls are not here: this is for watching, not editing.

    The bars sit above and below the visuals, never over them, because a GPU renderer draws in a
    native window that nothing can be painted on top of. So the same layout works on every tier.

    Keys (when this has focus, which it takes when shown): Escape exits, Space plays or pauses,
    Left and Right seek 5 seconds, Home returns to the start. The mouse can always exit through the
    button. The VisualHost is borrowed: it is added here and handed back by releaseHost().
*/
class FullscreenVisualsContent final : public juce::Component, private juce::Timer
{
public:
    FullscreenVisualsContent (rendering::VisualHost& host,
                              rendering::VisualController& visuals,
                              FullscreenActions actions,
                              juce::String songTitle);
    ~FullscreenVisualsContent() override;

    /** Called when the user asks to leave (Escape or the button). The owner closes the screen later,
        not from inside this call. */
    std::function<void()> onExit;

    /** The one-line renderer detail (which tier is running and why). */
    void setStatusText (const juce::String& text);

    /** Colours the bars and controls from the artwork palette (a faint wash of its accent in the bars,
        the accent on the sliders and buttons). The default palette leaves the plain look. The red Exit
        button never changes: it has to stay obvious. */
    void setTheme (const rendering::ThemePalette& palette);

    /** The bar colour for a palette: dark, with a wash of the accent that keeps white text at 7:1. */
    static juce::Colour barColourFor (const rendering::ThemePalette& palette);
    static juce::Colour defaultBarColour() { return juce::Colour (0xff101214); }
    juce::Colour getBarColour() const noexcept { return barColour; }

    /** Takes the visual host back out so it can return to the player. */
    void releaseHost();

    void resized() override;
    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void parentHierarchyChanged() override;

    static constexpr int topBarHeight = 52;
    static constexpr int bottomBarHeight = 64;
    static constexpr double seekStepSeconds = 5.0;

    // Exposed so tests can drive the same controls a user would.
    juce::TextButton& exitControl() noexcept { return exitButton; }
    juce::TextButton& playControl() noexcept { return playButton; }
    juce::Slider& positionControl() noexcept { return position; }
    juce::ToggleButton& effectsControl() noexcept { return effectsToggle; }
    juce::Slider& intensityControl() noexcept { return intensity; }
    juce::Rectangle<int> getVisualsArea() const noexcept { return visualsArea; }

private:
    void timerCallback() override { refresh(); }
    void refresh();
    void requestExit();

    rendering::VisualHost* host;
    rendering::VisualController& visuals;
    FullscreenActions actions;
    juce::String title;

    juce::Label titleLabel, statusLabel, timeLabel;
    juce::TextButton exitButton { "Exit fullscreen (Esc)" };
    juce::TextButton playButton { "Play" };
    juce::Slider position, intensity;
    juce::ToggleButton effectsToggle { "Effects" };
    juce::Rectangle<int> visualsArea;
    juce::Colour barColour = defaultBarColour();
    bool dragging = false;
    bool updating = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FullscreenVisualsContent)
};

/** The window that shows the content over the whole display (kiosk mode). It has no title bar, so
    the content's Exit button and Escape are the ways out; Alt+F4 / the system close also exits. */
class FullscreenVisualsWindow final : public juce::DocumentWindow
{
public:
    /** `content` is owned by the window. `nearComponent` picks the display (the main window's). */
    FullscreenVisualsWindow (std::unique_ptr<FullscreenVisualsContent> content,
                             juce::Component* nearComponent,
                             bool kioskMode = true);
    ~FullscreenVisualsWindow() override;

    FullscreenVisualsContent& getContent() noexcept
    {
        return *static_cast<FullscreenVisualsContent*> (getContentComponent());
    }

    std::function<void()> onExit;

    bool keyPressed (const juce::KeyPress& key) override { return getContent().keyPressed (key); }

    void closeButtonPressed() override
    {
        if (onExit)
            onExit();
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FullscreenVisualsWindow)
};
} // namespace snds
