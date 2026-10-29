#pragma once

#include "app/AppPaths.h"
#include "app/Logging.h"
#include "app/ReadinessProbe.h"
#include "model/AppState.h"
#include "ui/ImportScreen.h"
#include "ui/PlaceholderScreen.h"
#include "ui/ReadinessScreen.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace snds
{

/** Root content of the main window: owns the screens and switches between them by AppScreen. */
class MainComponent final : public juce::Component
{
public:
    MainComponent (std::shared_ptr<ReadinessProbe> readinessProbe, const AppPaths& paths, logging::LogService& log);

    void showScreen (model::AppScreen screen);
    model::AppScreen getScreen() const noexcept { return currentScreen; }

    /** Switches to song import and starts importing the file (used by the --open flag). */
    void openSong (const juce::File& file);

    void resized() override;

private:
    juce::Component& componentFor (model::AppScreen screen) noexcept;
    void logReadiness (const ReadinessReport& report);

    logging::LogService& log;
    ui::ReadinessScreen readinessScreen;
    ui::ImportScreen importScreen;
    ui::PlaceholderScreen playerScreen;
    model::AppScreen currentScreen = model::AppScreen::readiness;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace snds
