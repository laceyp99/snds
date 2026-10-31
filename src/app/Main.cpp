#include <juce_gui_basics/juce_gui_basics.h>

#include "app/AppInfo.h"
#include "app/AppOptions.h"
#include "app/AudioService.h"
#include "app/DataPaths.h"
#include "app/Logging.h"
#include "app/Settings.h"
#include "app/WorkerLocation.h"
#include "ui/MainComponent.h"

namespace snds
{
namespace
{
class MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow (const WorkerLocation& workerLocation,
                int readinessTimeoutMs,
                const std::optional<DataPaths>& paths,
                Settings* settings,
                AudioService* audio)
        : DocumentWindow (
              appinfo::name,
              juce::LookAndFeel::getDefaultLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId),
              DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new MainComponent (workerLocation, readinessTimeoutMs, paths, settings, audio), true);
        setResizable (true, false);
        setResizeLimits (720, 480, 10000, 10000);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    MainComponent& getMainComponent() { return *static_cast<MainComponent*> (getContentComponent()); }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};

class SndsApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return appinfo::name; }
    const juce::String getApplicationVersion() override { return appinfo::version(); }

    // Single-instance behaviour is undecided until cache and audio-device ownership exist.
    // Until then a second launch (for example a smoke run) must not silently hand off to an
    // already running instance and exit with 0.
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String&) override
    {
        options = AppOptions::parse (getCommandLineParameterArray());
        startServices();
        audio = std::make_unique<AudioService> (settings.get());

        const auto workerLocation = WorkerLocation::fromEnvironment (options.workerRoot);
        logInfo ("Worker folder: " + workerLocation.root.getFullPathName() + " (" + workerLocation.source + ")");

        mainWindow = std::make_unique<MainWindow> (
            workerLocation, options.readinessTimeoutSeconds * 1000, paths, settings.get(), audio.get());

        // Smoke runs check the window, not the sound card: they never open an audio device.
        if (! options.smokeExit && ! options.smokeExitAfterReadiness && ! options.noAudio)
            audio->start();

        if (options.openSong.isNotEmpty())
            mainWindow->getMainComponent().openCachedSong (options.openSong);

        if (options.exportDiagnostics != juce::File())
            mainWindow->getMainComponent().onReadinessChanged = [this] (const Readiness& readiness)
            { exportDiagnosticsAfterDelay (readiness); };
        else if (options.smokeExitAfterReadiness)
            mainWindow->getMainComponent().onReadinessChanged = [this] (const Readiness& readiness)
            { finishSmokeRun (readiness); };
        else if (options.smokeExit)
            juce::MessageManager::callAsync ([this] { finishSmokeRun ({}); });
    }

    void shutdown() override
    {
        mainWindow.reset(); // closes the audio dialog and unloads the engine before the device goes
        audio.reset();
        logInfo ("SNDS exiting");
        juce::Logger::setCurrentLogger (nullptr);
        logger.reset();
    }

    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted (const juce::String&) override {}

private:
    // Logging and settings must never stop the app from starting: failures are logged (or sent
    // to the debugger output when there is no log file) and the app carries on with defaults.
    void startServices()
    {
        paths = DataPaths::fromEnvironment();

        if (paths.has_value())
        {
            logger = std::make_unique<RotatingFileLogger> (paths->logsDirectory().getChildFile ("app.log"));

            if (logger->isOpen())
                juce::Logger::setCurrentLogger (logger.get());
            else
                logger.reset();
        }

        logInfo ("SNDS " + appinfo::version() + " starting (" + juce::SystemStats::getJUCEVersion() + ", "
                 + juce::SystemStats::getOperatingSystemName() + ")");

        for (const auto& problem : options.problems)
            logWarning ("Command line: " + problem);

        if (! paths.has_value())
        {
            logError ("Neither SNDS_HOME nor LOCALAPPDATA is set; settings and logs are unavailable.");
            return;
        }

        const auto homeSource = juce::SystemStats::getEnvironmentVariable (DataPaths::homeVariable, {}).trim().isEmpty()
                                  ? "default"
                                  : DataPaths::homeVariable;
        logInfo ("Data directory: " + paths->home.getFullPathName() + " (" + homeSource + ")");

        if (logger == nullptr)
            logWarning ("Could not open " + paths->logsDirectory().getChildFile ("app.log").getFullPathName());

        settings = std::make_unique<Settings> (paths->settingsFile());

        for (const auto& warning : settings->load().warnings)
            logWarning (warning);
    }

    // Automation exit. The basic checks: the window is on screen, every argument parsed cleanly,
    // and the linked JUCE matches the pin. After a readiness check, the exit code also reports
    // its state (see AppOptions::smokeExitAfterReadiness).
    // --export-diagnostics: after the first readiness check, give the window a few seconds (so the
    // visuals and device have a status to report), then write the zip and quit.
    void exportDiagnosticsAfterDelay (const Readiness& readiness)
    {
        if (readiness.state == Readiness::State::checking)
            return;

        juce::Timer::callAfterDelay (
            3000,
            [this]
            {
                if (mainWindow == nullptr)
                    return;

                mainWindow->getMainComponent().exportDiagnosticsTo (
                    options.exportDiagnostics,
                    [this] (const juce::Result& result)
                    {
                        logInfo ("Diagnostics export: "
                                 + (result.wasOk() ? juce::String ("ok") : result.getErrorMessage()));
                        setApplicationReturnValue (result.wasOk() ? 0 : 1);
                        quit();
                    });
            });
    }

    void finishSmokeRun (std::optional<Readiness> readiness)
    {
        const auto ok =
            mainWindow != nullptr && mainWindow->isShowing() && options.problems.isEmpty() && appinfo::juceMatchesPin();
        auto exitCode = ok ? 0 : 1;

        if (ok && readiness.has_value())
        {
            switch (readiness->state)
            {
                case Readiness::State::ready:
                    exitCode = 0;
                    break;
                case Readiness::State::notSetUp:
                    exitCode = 10;
                    break;
                case Readiness::State::modelMissing:
                    exitCode = 11;
                    break;
                case Readiness::State::checking:
                case Readiness::State::error:
                    exitCode = 12;
                    break;
            }

            logInfo ("Smoke run readiness state: " + Readiness::stateName (readiness->state));
        }

        logInfo (juce::String ("Smoke run ") + (ok ? "passed" : "failed") + ", exit code " + juce::String (exitCode));
        setApplicationReturnValue (exitCode);
        quit();
    }

    AppOptions options;
    std::optional<DataPaths> paths;
    std::unique_ptr<RotatingFileLogger> logger;
    std::unique_ptr<Settings> settings;
    std::unique_ptr<AudioService> audio;
    std::unique_ptr<MainWindow> mainWindow;
};
} // namespace
} // namespace snds

START_JUCE_APPLICATION (snds::SndsApplication)
