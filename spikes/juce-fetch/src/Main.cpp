#include <juce_gui_basics/juce_gui_basics.h>

#include "SpikeInfo.h"

namespace
{
class MainComponent final : public juce::Component
{
public:
    MainComponent() { setSize (480, 200); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black);
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (18.0f));
        g.drawFittedText ("SNDS JUCE fetch spike\n" + juce::SystemStats::getJUCEVersion(),
                          getLocalBounds().reduced (16),
                          juce::Justification::centred,
                          2);
    }
};

class MainWindow final : public juce::DocumentWindow
{
public:
    explicit MainWindow (const juce::String& name)
        : DocumentWindow (name, juce::Colours::black, DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new MainComponent(), true);
        setResizable (true, true);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class SpikeApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String&) override
    {
        mainWindow = std::make_unique<MainWindow> (getApplicationName());

        // --smoke-exit: prove the GUI runtime starts, then quit with 0 only if the
        // window is showing and the compiled JUCE version matches the pin.
        if (getCommandLineParameterArray().contains ("--smoke-exit"))
        {
            juce::MessageManager::callAsync ([this]
            {
                const auto ok = mainWindow != nullptr && mainWindow->isShowing()
                             && snds::spike::juceVersionMatchesPin();
                setApplicationReturnValue (ok ? 0 : 1);
                quit();
            });
        }
    }

    void shutdown() override { mainWindow.reset(); }
    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted (const juce::String&) override {}

private:
    std::unique_ptr<MainWindow> mainWindow;
};
} // namespace

START_JUCE_APPLICATION (SpikeApplication)
