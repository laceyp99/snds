#include "app/AppPaths.h"
#include "app/Logging.h"
#include "app/MainComponent.h"
#include "app/ReadinessProbe.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace snds
{

namespace
{
/**
    The file after "--open" on the command line, or an empty File. Opens a song straight into the
    import flow; used for integration testing and screenshots. Relative paths use the working directory.
*/
juce::File findOpenArgument (const juce::StringArray& arguments)
{
    const auto index = arguments.indexOf ("--open");

    if (index < 0 || index + 1 >= arguments.size())
        return {};

    const auto path = arguments[index + 1].trim().unquoted();

    if (path.isEmpty())
        return {};

    return juce::File::getCurrentWorkingDirectory().getChildFile (path);
}
} // namespace

class MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow (const juce::String& name, std::unique_ptr<juce::Component> content)
        : DocumentWindow (name,
                          juce::Desktop::getInstance().getDefaultLookAndFeel().findColour (
                              juce::ResizableWindow::backgroundColourId),
                          DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (content.release(), true);
        setResizable (true, true);
        setResizeLimits (720, 520, 8192, 8192);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};

//==============================================================================
class SndsApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "SNDS"; }
    const juce::String getApplicationVersion() override { return SNDS_VERSION_STRING; }

    // One instance at a time: the logs and cache assume a single writer process.
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise (const juce::String&) override
    {
        paths = std::make_unique<AppPaths> (AppPaths::forCurrentUser());
        const auto logsReady = paths->ensureDirectory (AppDirectory::logs);

        logService = std::make_unique<logging::LogService> (paths->getDirectory (AppDirectory::logs),
                                                            logging::PathRedactor::forCurrentUser());
        loggerBridge = std::make_unique<logging::JuceLoggerBridge> (*logService);
        juce::Logger::setCurrentLogger (loggerBridge.get());

        logService->info (logging::LogCategory::app,
                          "SNDS " + getApplicationVersion() + " starting (" + juce::SystemStats::getJUCEVersion() + ", "
                              + juce::SystemStats::getOperatingSystemName() + ")");
        logService->info (logging::LogCategory::app, "Data root: " + paths->getRoot().getFullPathName());

        if (logsReady.failed())
            juce::Logger::outputDebugString ("SNDS: log directory unavailable: " + logsReady.getErrorMessage());

        auto probe = std::make_shared<LocalReadinessProbe> (LocalEnvironment::capture (*paths));
        auto content = std::make_unique<MainComponent> (probe, *paths, *logService);
        mainComponent = content.get();
        mainWindow = std::make_unique<MainWindow> (getApplicationName(), std::move (content));

        if (const auto file = findOpenArgument (getCommandLineParameterArray()); file != juce::File())
            mainComponent->openSong (file);
    }

    void shutdown() override
    {
        mainComponent = nullptr;
        mainWindow = nullptr;

        if (logService != nullptr)
            logService->info (logging::LogCategory::app, "SNDS shutting down");

        juce::Logger::setCurrentLogger (nullptr);
        loggerBridge = nullptr;
        logService = nullptr;
        paths = nullptr;
    }

    void systemRequestedQuit() override { quit(); }

    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        if (mainWindow != nullptr)
            mainWindow->toFront (true);

        if (const auto file = findOpenArgument (juce::StringArray::fromTokens (commandLine, true));
            mainComponent != nullptr && file != juce::File())
            mainComponent->openSong (file);
    }

    void unhandledException (const std::exception* e, const juce::String& sourceFilename, int lineNumber) override
    {
        if (logService != nullptr)
            logService->error (logging::LogCategory::app,
                               "Unhandled exception at " + sourceFilename + ":" + juce::String (lineNumber) + ": "
                                   + (e != nullptr ? juce::String (e->what()) : juce::String ("unknown")));
    }

private:
    std::unique_ptr<AppPaths> paths;
    std::unique_ptr<logging::LogService> logService;
    std::unique_ptr<logging::JuceLoggerBridge> loggerBridge;
    std::unique_ptr<MainWindow> mainWindow;
    MainComponent* mainComponent = nullptr; // owned by mainWindow
};

} // namespace snds

START_JUCE_APPLICATION (snds::SndsApplication)
