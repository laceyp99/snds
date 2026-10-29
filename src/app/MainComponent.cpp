#include "app/MainComponent.h"

namespace snds
{

using model::AppScreen;

MainComponent::MainComponent (std::shared_ptr<ReadinessProbe> readinessProbe,
                              const AppPaths& paths,
                              logging::LogService& logService)
    : log (logService),
      readinessScreen (std::move (readinessProbe), paths.getDirectory (AppDirectory::logs)),
      importScreen (paths, logService),
      playerScreen ("Player", "Transport, stem mixing, and reactive artwork appear here once a song has been prepared.")
{
    readinessScreen.onContinue = [this] { showScreen (AppScreen::songImport); };
    readinessScreen.onReportReady = [this] (const ReadinessReport& report) { logReadiness (report); };

    importScreen.onBackToReadiness = [this] { showScreen (AppScreen::readiness); };
    importScreen.onPreviewPlayer = [this] { showScreen (AppScreen::player); };
    playerScreen.addNavigation ("Back to import", [this] { showScreen (AppScreen::songImport); });

    addChildComponent (readinessScreen);
    addChildComponent (importScreen);
    addChildComponent (playerScreen);

    setSize (1024, 720);
    showScreen (AppScreen::readiness);
    readinessScreen.refresh();
}

void MainComponent::showScreen (AppScreen screen)
{
    if (screen != currentScreen)
        log.info (logging::LogCategory::app,
                  juce::String ("Screen ") + model::toString (currentScreen) + " -> " + model::toString (screen));

    currentScreen = screen;

    for (auto candidate : { AppScreen::readiness, AppScreen::songImport, AppScreen::player })
        componentFor (candidate).setVisible (candidate == screen);
}

void MainComponent::openSong (const juce::File& file)
{
    showScreen (AppScreen::songImport);
    importScreen.openSource (file);
}

void MainComponent::resized()
{
    const auto bounds = getLocalBounds();
    readinessScreen.setBounds (bounds);
    importScreen.setBounds (bounds);
    playerScreen.setBounds (bounds);
}

juce::Component& MainComponent::componentFor (AppScreen screen) noexcept
{
    switch (screen)
    {
        case AppScreen::readiness:
            return readinessScreen;
        case AppScreen::songImport:
            return importScreen;
        case AppScreen::player:
            return playerScreen;
    }

    return readinessScreen;
}

void MainComponent::logReadiness (const ReadinessReport& report)
{
    log.info (logging::LogCategory::app,
              juce::String ("Readiness: ")
                  + (report.isSeparationAvailable() ? "local setup complete" : "setup needed"));

    for (const auto& check : report.checks)
        log.info (logging::LogCategory::app, "  " + check.id + ": " + toString (check.status) + " - " + check.detail);
}

} // namespace snds
