#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace snds::ui
{

/**
    A titled screen with explanatory text and navigation buttons. Used for shell screens whose real
    content arrives with later features (import, player).
*/
class PlaceholderScreen final : public juce::Component
{
public:
    PlaceholderScreen (const juce::String& title, const juce::String& body);

    /** Adds a navigation button at the bottom of the screen, laid out left to right. */
    void addNavigation (const juce::String& label, std::function<void()> onClick);

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    juce::Label titleLabel;
    juce::Label bodyLabel;
    std::vector<std::unique_ptr<juce::TextButton>> buttons;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlaceholderScreen)
};

} // namespace snds::ui
