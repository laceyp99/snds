#include "ui/ArtworkConfirmationPanel.h"

namespace snds
{
namespace
{
constexpr int padding = 16;
constexpr int previewSize = 220;
} // namespace

ArtworkConfirmationPanel::ArtworkConfirmationPanel()
{
    heading.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    message.setFont (juce::FontOptions (14.0f));
    message.setMinimumHorizontalScale (1.0f);

    useThisButton.onClick = [this] { useDetected(); };
    chooseButton.onClick = [this] { chooseImage(); };
    defaultButton.onClick = [this] { useDefault(); };

    for (auto* child :
         std::initializer_list<juce::Component*> { &heading, &message, &useThisButton, &chooseButton, &defaultButton })
        addAndMakeVisible (child);

    setSize (560, previewSize + 2 * padding);
}

void ArtworkConfirmationPanel::show (const juce::String& songName,
                                     const juce::String& seed,
                                     std::optional<ImageData> detectedArtwork)
{
    seedText = seed;
    detected = std::move (detectedArtwork);
    preview = {};

    if (detected)
        preview = juce::ImageFileFormat::loadFrom (detected->bytes.getData(), detected->bytes.getSize());

    if (! preview.isValid())
        detected.reset(); // bytes that do not decode are as good as none

    if (detected)
    {
        heading.setText ("Artwork found in " + songName, juce::dontSendNotification);
        showMessage ("Use this picture, pick another image, or use the default artwork.");
    }
    else
    {
        preview = makeFallbackArtwork (seedText, previewSize);
        heading.setText ("No artwork found in " + songName, juce::dontSendNotification);
        showMessage ("You can choose an image or use the default artwork shown here.");
    }

    useThisButton.setVisible (detected.has_value());
    resized();
    repaint();
}

void ArtworkConfirmationPanel::paint (juce::Graphics& g)
{
    g.setColour (findColour (juce::ResizableWindow::backgroundColourId).brighter (0.15f));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);

    const auto area = juce::Rectangle<int> (padding, padding, previewSize, previewSize);

    if (preview.isValid())
        g.drawImage (preview, area.toFloat(), juce::RectanglePlacement::centred);
}

void ArtworkConfirmationPanel::resized()
{
    auto bounds = getLocalBounds().reduced (padding);
    bounds.removeFromLeft (previewSize + padding);

    heading.setBounds (bounds.removeFromTop (28));
    message.setBounds (bounds.removeFromTop (48));
    bounds.removeFromTop (8);

    for (auto* button : { &useThisButton, &chooseButton, &defaultButton })
    {
        if (button->isVisible())
        {
            button->setBounds (bounds.removeFromTop (32).withWidth (200));
            bounds.removeFromTop (8);
        }
    }
}

void ArtworkConfirmationPanel::showMessage (const juce::String& text)
{
    message.setText (text, juce::dontSendNotification);
}

void ArtworkConfirmationPanel::decide (ArtworkChoice choice)
{
    if (onChosen != nullptr)
        onChosen (std::move (choice));
}

void ArtworkConfirmationPanel::useDetected()
{
    if (detected)
        decide (makeEmbeddedChoice (*detected));
}

void ArtworkConfirmationPanel::useDefault()
{
    decide (makeFallbackChoice (seedText));
}

void ArtworkConfirmationPanel::chooseImage()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose artwork", juce::File(), "*.png;*.jpg;*.jpeg;*.gif");

    chooser->launchAsync (
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safe = juce::Component::SafePointer<ArtworkConfirmationPanel> (this)] (const juce::FileChooser& fc)
        {
            if (safe == nullptr || fc.getResult() == juce::File())
                return; // cancelled: keep the panel as it is

            juce::String error;

            if (auto choice = loadArtworkFile (fc.getResult(), error))
                safe->decide (std::move (*choice));
            else
                safe->showMessage (error);
        });
}
} // namespace snds
