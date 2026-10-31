#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "app/ErrorReport.h"
#include "app/MixService.h"
#include "app/PlaybackController.h"
#include "app/SongSession.h"
#include "rendering/ArtworkPalette.h"
#include "rendering/VisualController.h"
#include "rendering/VisualHost.h"
#include "ui/FullscreenVisuals.h"
#include "ui/TransportBar.h"

#include <functional>
#include <optional>

namespace snds
{
/** The player: what is open, with its reactive artwork, tempo, compute device, and the state of its
    stems and original, above the transport bar. The original-missing state and Locate Original
    show here; whether A/B works is the transport bar's business.

    The artwork is a rendering::VisualHost driven by a VisualController. Each tick (30 Hz) the panel
    takes the newest analysis from the current MixProcessor (through the MixService) while the song
    plays, and a silent one otherwise, so the picture settles when the music stops. Selecting the
    Original makes it static. The host is started once the panel is on screen (the GPU tier needs a
    visible window). Intensity and effects on/off are session settings, not mix state: nothing here
    is persisted and the mix reset does not touch them.

    Fullscreen moves the same host into a FullscreenVisualsWindow and back.
*/
class SongPanel final : public juce::Component, private juce::Timer
{
public:
    /** The overrides and factory exist for tests (forced failures, fake renderers). */
    explicit SongPanel (rendering::RenderOverrides overrides = rendering::RenderOverrides::fromEnvironment(),
                        rendering::VisualHost::RendererFactory factory = {});
    ~SongPanel() override;

    /** Shows the song, or the empty state for nullopt. */
    void setSong (const std::optional<OpenSong>& song);

    /** Called when the user asks to locate the original (the button is shown only when it is missing). */
    std::function<void()> onLocateOriginal;

    /** The transport bar plays and shows this controller; null leaves it idle. */
    void setController (PlaybackController* newController)
    {
        playback = newController;
        transport.setController (newController);
        viewSource = newController != nullptr
                       ? std::function<PlaybackView()> ([newController] { return newController->view(); })
                       : std::function<PlaybackView()>();
    }

    /** Where the visuals learn whether music plays and whether the Original is selected. Set by
        setController; tests replace it. */
    std::function<PlaybackView()> viewSource;

    /** Called (message thread) when the renderer steps down a tier, with the reason as plain strings, so
        the app can show the shared notice and log it. Skipped tiers that simply do not exist are not reported. */
    std::function<void (const RendererFallbackInfo&)> onRendererFallback;

    /** Where the visuals read the analysis from (the newest processor); null means silence. */
    void setMixService (MixService* mix) noexcept { mixService = mix; }

    TransportBar& getTransport() noexcept { return transport; }
    rendering::VisualHost& getVisualHost() noexcept { return visualHost; }
    rendering::VisualController& getVisualController() noexcept { return visuals; }

    /** Locating needs the session to be idle. */
    void setLocateEnabled (bool enabled);

    //==========================================================================
    // Visuals.

    /** Whether the host may start now. By default: the panel is showing in a window. Tests replace it. */
    std::function<bool()> canStartVisuals;

    /** The clock the easing uses (milliseconds). Real time by default; tests drive it. */
    std::function<double()> clockMs = [] { return juce::Time::getMillisecondCounterHiRes(); };

    /** Whether the fullscreen window uses the system's kiosk mode (covers the display). Tests turn it
        off to get an ordinary window. */
    bool useKioskMode = true;

    /** One visuals tick (the timer calls it): starts the host when allowed, follows the Original/Stem
        Mix choice, feeds the controller, and refreshes the detail line. Public for tests. */
    void tickVisuals();

    /** The renderer detail line shown under the artwork. */
    const juce::String& getVisualsDetail() const noexcept { return detailText; }

    void enterFullscreen();
    void exitFullscreen();
    juce::ToggleButton& effectsControl() noexcept { return effectsToggle; }
    juce::Slider& intensityControl() noexcept { return intensitySlider; }
    juce::TextButton& fullscreenControl() noexcept { return fullscreenButton; }
    bool isFullscreen() const noexcept { return fullscreen != nullptr; }
    FullscreenVisualsWindow* getFullscreenWindow() noexcept { return fullscreen.get(); }

    /** The accent palette taken from the artwork of the open song (or the default styling when there
        is no usable artwork). The player's accents and the fullscreen bars follow it. */
    const rendering::ThemePalette& getTheme() const noexcept { return theme; }

    void paint (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override;

    static constexpr int artworkSize = 160;

private:
    void timerCallback() override { tickVisuals(); }
    void refreshTheme();
    void applyTheme();
    juce::Colour basePanelColour() const;
    void syncVisualControls();
    void updateDetail();
    bool isHostFullyInView() const;

    std::optional<OpenSong> song;
    juce::Image artwork;
    PlaybackController* playback = nullptr;
    MixService* mixService = nullptr;

    juce::TextButton locateButton { "Locate Original..." };
    TransportBar transport;

    rendering::VisualHost visualHost;
    rendering::VisualController visuals { visualHost };
    juce::ToggleButton effectsToggle { "Effects" };
    juce::Slider intensitySlider;
    juce::Label intensityLabel;
    juce::TextButton fullscreenButton { "Fullscreen" };
    juce::Label detailLabel;
    juce::String detailText;
    int ticksSinceDetail = 1000;
    std::unique_ptr<FullscreenVisualsWindow> fullscreen;
    rendering::ThemePalette theme;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SongPanel)
};

/** "3:07" for a duration in seconds. */
juce::String formatDuration (double seconds);
} // namespace snds
