#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "model/Artwork.h"

#include <functional>
#include <optional>

namespace snds
{
/** The artwork step of importing a song: shows the detected cover (or the default one when the
    file has none) and asks for a decision with "Use this", "Choose image...", and "Use default
    artwork". onChosen fires once per decision, on the message thread.
*/
class ArtworkConfirmationPanel final : public juce::Component
{
public:
    ArtworkConfirmationPanel();

    /** `seed` (normally the song's file name) makes the default artwork specific to the song. */
    void show (const juce::String& songName, const juce::String& seed, std::optional<ImageData> detected);

    /** The decision the buttons would produce for "Use this", if there is detected art. */
    bool hasDetectedArtwork() const noexcept { return detected.has_value(); }

    std::function<void (ArtworkChoice)> onChosen;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void useDetected();
    void useDefault();
    void chooseImage();
    void showMessage (const juce::String& text);
    void decide (ArtworkChoice choice);

    juce::String seedText;
    std::optional<ImageData> detected;
    juce::Image preview;

    juce::Label heading, message;
    juce::TextButton useThisButton { "Use this" }, chooseButton { "Choose image..." },
        defaultButton { "Use default artwork" };
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArtworkConfirmationPanel)
};
} // namespace snds
