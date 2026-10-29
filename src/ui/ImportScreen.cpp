#include "ui/ImportScreen.h"

#include "app/EmbeddedArtwork.h"
#include "rendering/FallbackArtwork.h"

namespace snds::ui
{

using model::ImportStage;

namespace
{
const juce::Colour problemColour (0xffef6c00);
const juce::Colour okColour (0xff4caf50);

juce::String shortHash (const model::SourceFingerprint& fingerprint)
{
    return "sha256:" + fingerprint.sha256Hex.substring (0, 12) + "..." + fingerprint.sha256Hex.getLastCharacters (6);
}

juce::String withThousands (juce::int64 value)
{
    auto digits = juce::String (value);

    for (auto i = digits.length() - 3; i > 0; i -= 3)
        digits = digits.substring (0, i) + "," + digits.substring (i);

    return digits;
}
} // namespace

//==============================================================================
/** Draws the selected artwork aspect-fitted in a rounded panel. */
class ImportScreen::ArtworkView final : public juce::Component
{
public:
    void setImage (const juce::Image& newImage)
    {
        image = newImage;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        g.setColour (findColour (juce::ResizableWindow::backgroundColourId).brighter (0.08f));
        g.fillRoundedRectangle (bounds, 8.0f);

        if (! image.isValid())
        {
            g.setColour (findColour (juce::Label::textColourId).withAlpha (0.5f));
            g.setFont (juce::FontOptions (15.0f));
            g.drawText ("No artwork", getLocalBounds(), juce::Justification::centred);
            return;
        }

        const auto target = bounds.reduced (8.0f);
        g.drawImage (image,
                     target,
                     juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize
                         | juce::RectanglePlacement::stretchToFit);
    }

private:
    juce::Image image;
};

/** Everything the background inspection produces for one file. */
struct ImportScreen::Inspection
{
    ImportResult result;
    std::optional<model::Artwork> embedded;
    juce::String artworkNote;
    model::Artwork fallback;
};

//==============================================================================
ImportScreen::ImportScreen (const AppPaths& paths, logging::LogService& logService)
    : log (logService),
      memo (std::make_shared<FingerprintMemo> (
          paths.getDirectory (AppDirectory::settings).getChildFile (FingerprintMemo::defaultFileName))),
      fingerprinter (memo),
      artworkView (std::make_unique<ArtworkView>())
{
    titleLabel.setFont (juce::FontOptions (26.0f, juce::Font::bold));
    addAndMakeVisible (titleLabel);

    messageLabel.setFont (juce::FontOptions (16.0f));
    messageLabel.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (messageLabel);

    detailsLabel.setFont (juce::FontOptions (15.0f));
    detailsLabel.setJustificationType (juce::Justification::topLeft);
    addChildComponent (detailsLabel);

    fingerprintLabel.setFont (juce::FontOptions (14.0f));
    fingerprintLabel.setJustificationType (juce::Justification::topLeft);
    addChildComponent (fingerprintLabel);

    artworkCaption.setFont (juce::FontOptions (14.0f));
    artworkCaption.setJustificationType (juce::Justification::topLeft);
    addChildComponent (artworkCaption);

    addChildComponent (*artworkView);

    openButton.onClick = [this] { chooseSourceFile(); };
    confirmButton.onClick = [this]
    {
        artwork.confirm();
        log.info (logging::LogCategory::app,
                  juce::String ("Import: artwork confirmed (") + model::toString (artwork.getSelectedOrigin())
                      + "): " + artwork.getSelected().description);
        setStage (ImportStage::prepare);
    };
    replaceButton.onClick = [this] { chooseReplacementArtwork(); };
    fallbackButton.onClick = [this]
    {
        artwork.selectFallback();
        refreshContent();
    };
    embeddedButton.onClick = [this]
    {
        artwork.selectEmbedded();
        refreshContent();
    };
    changeArtworkButton.onClick = [this] { setStage (ImportStage::confirmArtwork); };
    backButton.onClick = [this]
    {
        if (onBackToReadiness)
            onBackToReadiness();
    };
    previewButton.onClick = [this]
    {
        if (onPreviewPlayer)
            onPreviewPlayer();
    };

    for (auto* button : { &openButton,
                          &confirmButton,
                          &replaceButton,
                          &fallbackButton,
                          &embeddedButton,
                          &changeArtworkButton,
                          &backButton,
                          &previewButton })
        addChildComponent (*button);

    setStage (ImportStage::chooseSource);
}

ImportScreen::~ImportScreen()
{
    stopTimer();
    fingerprinter.cancel();
    workPool.removeAllJobs (true, 10000);
}

void ImportScreen::openSource (const juce::File& file)
{
    const auto thisGeneration = ++generation;
    fingerprinter.cancel();

    currentFile = file;
    source.reset();
    fingerprint.reset();
    artwork = {};
    artworkNote.clear();
    artworkNoteIsError = false;
    setStage (ImportStage::inspecting);

    log.info (logging::LogCategory::app, "Import: inspecting " + file.getFullPathName());

    workPool.addJob (
        [file, thisGeneration, safeThis = SafePointer<ImportScreen> (this)]
        {
            Inspection inspection;
            inspection.result = inspectSource (file);

            if (inspection.result.wasOk())
            {
                auto extracted = extractEmbeddedArtwork (*inspection.result.source);
                inspection.embedded = std::move (extracted.artwork);
                inspection.artworkNote = extracted.note;

                inspection.fallback.image = rendering::generateFallbackArtwork (
                    rendering::fallbackArtworkSeed (file.getFileNameWithoutExtension()));
                inspection.fallback.origin = model::ArtworkOrigin::fallback;
                inspection.fallback.description = "Generated artwork (derived from the file name)";
            }

            juce::MessageManager::callAsync (
                [safeThis, thisGeneration, inspection]
                {
                    if (auto* screen = safeThis.getComponent())
                        screen->applyInspection (thisGeneration, inspection);
                });
        });
}

void ImportScreen::applyInspection (int generationOfResult, const Inspection& inspection)
{
    if (generationOfResult != generation)
        return;

    if (! inspection.result.wasOk())
    {
        problem = inspection.result.error;
        log.warning (logging::LogCategory::app,
                     juce::String ("Import: rejected (") + toString (problem.code) + "): " + problem.message);
        setStage (ImportStage::problem);
        return;
    }

    source = inspection.result.source;
    artwork = model::ArtworkChoice (inspection.embedded, inspection.fallback);
    artworkNote = inspection.artworkNote;

    log.info (logging::LogCategory::app, "Import: accepted " + source->getSummary());
    log.info (logging::LogCategory::app,
              "Import: artwork " + (inspection.embedded.has_value() ? inspection.embedded->description : "none")
                  + (artworkNote.isNotEmpty() ? " - " + artworkNote : juce::String()));

    fingerprinter.start (source->file,
                         [safeThis = SafePointer<ImportScreen> (this),
                          generationOfResult] (const juce::File&, const FingerprintOutcome& outcome)
                         {
                             juce::MessageManager::callAsync (
                                 [safeThis, generationOfResult, outcome]
                                 {
                                     if (auto* screen = safeThis.getComponent())
                                         screen->applyFingerprint (generationOfResult, outcome);
                                 });
                         });
    startTimerHz (10);

    setStage (ImportStage::confirmArtwork);
}

void ImportScreen::applyFingerprint (int generationOfResult, const FingerprintOutcome& outcome)
{
    if (generationOfResult != generation || outcome.status == FingerprintOutcome::Status::cancelled)
        return;

    fingerprint = outcome;

    if (outcome.status == FingerprintOutcome::Status::completed)
    {
        auto line = "Import: fingerprint " + outcome.fingerprint.getContentHash() + " ("
                    + juce::String (outcome.fingerprint.sizeBytes) + " bytes)";

        if (outcome.fromMemo)
            line << ", reused from the verified memo";
        else if (outcome.hashSeconds > 0.0)
            line << ", hashed in " << juce::String (outcome.hashSeconds, 3) << " s ("
                 << juce::String (static_cast<double> (outcome.fingerprint.sizeBytes) / (1024.0 * 1024.0)
                                      / outcome.hashSeconds,
                                  0)
                 << " MiB/s)";

        log.info (logging::LogCategory::app, line);
    }
    else
    {
        log.warning (logging::LogCategory::app, "Import: fingerprint failed: " + outcome.error);
    }

    refreshContent();
}

void ImportScreen::timerCallback()
{
    if (! fingerprinter.isRunning())
        stopTimer();

    fingerprintLabel.setText (describeFingerprint (stage == ImportStage::prepare), juce::dontSendNotification);
}

//==============================================================================
void ImportScreen::chooseSourceFile()
{
    const auto startDirectory = currentFile != juce::File()
                                    ? currentFile.getParentDirectory()
                                    : juce::File::getSpecialLocation (juce::File::userMusicDirectory);

    chooser = std::make_unique<juce::FileChooser> ("Open a song (WAV or MP3)", startDirectory, sourceFilePatterns);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safeThis = SafePointer<ImportScreen> (this)] (const juce::FileChooser& fileChooser)
                          {
                              const auto chosen = fileChooser.getResult();

                              if (auto* screen = safeThis.getComponent(); screen != nullptr && chosen != juce::File())
                                  screen->openSource (chosen);
                          });
}

void ImportScreen::chooseReplacementArtwork()
{
    const auto startDirectory = currentFile != juce::File()
                                    ? currentFile.getParentDirectory()
                                    : juce::File::getSpecialLocation (juce::File::userPicturesDirectory);

    chooser = std::make_unique<juce::FileChooser> ("Choose replacement artwork (PNG or JPEG)",
                                                   startDirectory,
                                                   artworkFilePatterns);
    chooser->launchAsync (
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safeThis = SafePointer<ImportScreen> (this)] (const juce::FileChooser& fileChooser)
        {
            auto* screen = safeThis.getComponent();
            const auto chosen = fileChooser.getResult();

            if (screen == nullptr || chosen == juce::File())
                return;

            screen->workPool.addJob (
                [safeThis, chosen, thisGeneration = screen->generation]
                {
                    const auto loaded = loadReplacementArtwork (chosen);

                    juce::MessageManager::callAsync (
                        [safeThis, thisGeneration, loaded]
                        {
                            auto* target = safeThis.getComponent();

                            if (target == nullptr || thisGeneration != target->generation)
                                return;

                            if (loaded.artwork.has_value())
                            {
                                target->artwork.selectReplacement (*loaded.artwork);
                                target->artworkNote.clear();
                                target->artworkNoteIsError = false;
                                target->log.info (logging::LogCategory::app,
                                                  "Import: replacement artwork " + loaded.artwork->description);
                            }
                            else
                            {
                                target->artworkNote = loaded.error;
                                target->artworkNoteIsError = true;
                                target->log.warning (logging::LogCategory::app,
                                                     "Import: replacement artwork rejected: " + loaded.error);
                            }

                            target->refreshContent();
                        });
                });
        });
}

//==============================================================================
void ImportScreen::setStage (ImportStage newStage)
{
    if (newStage != stage)
        log.info (logging::LogCategory::app,
                  juce::String ("Import stage ") + model::toString (stage) + " -> " + model::toString (newStage));

    stage = newStage;
    refreshContent();
}

juce::String ImportScreen::describeSource() const
{
    if (! source.has_value())
        return {};

    juce::String text;
    text << "File:  " << source->file.getFileName() << "\n"
         << "Folder:  " << source->file.getParentDirectory().getFullPathName() << "\n"
         << "Format:  " << model::toString (source->format) << ", " << source->encoding << "\n"
         << "Sample rate:  " << model::formatSampleRate (source->sampleRate) << ",  " << source->getChannelDescription()
         << "\n"
         << "Length:  " << model::formatDuration (source->getLengthSeconds()) << "  ("
         << withThousands (source->lengthInFrames) << " frames)\n"
         << "Size:  " << juce::File::descriptionOfSizeInBytes (source->fileSizeBytes);

    if (source->mp3.has_value())
    {
        text << "\nGapless info:  ";

        if (source->mp3->encoderDelay >= 0)
            text << "encoder delay " << source->mp3->encoderDelay << ", padding " << source->mp3->encoderPadding
                 << " samples (LAME tag)";
        else
            text << "none (no LAME tag)";
    }

    return text;
}

juce::String ImportScreen::describeFingerprint (bool fullHash) const
{
    if (fingerprint.has_value())
    {
        if (fingerprint->status != FingerprintOutcome::Status::completed)
            return "Fingerprint failed: " + fingerprint->error;

        const auto& value = fingerprint->fingerprint;
        auto text = "Fingerprint:  " + (fullHash ? value.getContentHash() : shortHash (value)) + "  ("
                    + withThousands (value.sizeBytes) + " bytes)";

        if (fingerprint->fromMemo)
            text << "\nReused: the file is unchanged since it was last verified.";
        else
            text << "\nComputed in " << juce::String (fingerprint->hashSeconds, 2) << " s.";

        return text;
    }

    if (fingerprinter.isRunning())
        return "Fingerprint:  hashing... " + juce::String (juce::roundToInt (fingerprinter.getProgress() * 100.0f))
               + "%";

    return "Fingerprint:  waiting";
}

void ImportScreen::refreshContent()
{
    const auto isConfirm = stage == ImportStage::confirmArtwork;
    const auto isPrepare = stage == ImportStage::prepare;
    const auto hasSource = isConfirm || isPrepare;

    switch (stage)
    {
        case ImportStage::chooseSource:
            titleLabel.setText ("Open a song", juce::dontSendNotification);
            messageLabel.setText ("Choose one WAV or MP3 file. SNDS checks it, shows its artwork for you to confirm, "
                                  "and prepares four stems (vocals, drums, bass, other) locally.\n\n"
                                  "Supported: WAV (16/24/32-bit PCM, 32/64-bit float) and MP3 (44.1/48/32 kHz), mono "
                                  "or stereo, up to 192 kHz. The file stays where it is; SNDS never copies it.",
                                  juce::dontSendNotification);
            openButton.setButtonText ("Open song...");
            break;

        case ImportStage::inspecting:
            titleLabel.setText ("Checking the file", juce::dontSendNotification);
            messageLabel.setText ("Checking \"" + currentFile.getFileName() + "\"...", juce::dontSendNotification);
            break;

        case ImportStage::problem:
            titleLabel.setText ("This file can't be used", juce::dontSendNotification);
            messageLabel.setText (problem.message + "\n\n" + problem.action, juce::dontSendNotification);
            openButton.setButtonText ("Choose another file...");
            break;

        case ImportStage::confirmArtwork:
            titleLabel.setText ("Confirm the artwork", juce::dontSendNotification);
            messageLabel.setText ("The artwork reacts to your mix during playback. Use it as is, replace it with an "
                                  "image, or use generated artwork.",
                                  juce::dontSendNotification);
            openButton.setButtonText ("Open another song...");
            break;

        case ImportStage::prepare:
            titleLabel.setText ("Ready to prepare", juce::dontSendNotification);
            messageLabel.setText ("Source and artwork are confirmed. Stem separation starts here once the worker is "
                                  "connected; it is not available in this build yet.",
                                  juce::dontSendNotification);
            openButton.setButtonText ("Open another song...");
            break;
    }

    messageLabel.setColour (juce::Label::textColourId,
                            stage == ImportStage::problem ? problemColour.brighter (0.3f)
                                                          : findColour (juce::Label::textColourId));

    detailsLabel.setText (describeSource(), juce::dontSendNotification);
    fingerprintLabel.setText (describeFingerprint (isPrepare), juce::dontSendNotification);
    fingerprintLabel.setColour (juce::Label::textColourId,
                                fingerprint.has_value() && fingerprint->status == FingerprintOutcome::Status::completed
                                    ? okColour.brighter (0.4f)
                                    : findColour (juce::Label::textColourId).withAlpha (0.8f));

    const auto& selected = artwork.getSelected();
    artworkView->setImage (hasSource ? selected.image : juce::Image());

    auto caption = selected.description;

    if (isConfirm && artworkNote.isNotEmpty())
        caption << "\n" << artworkNote;

    artworkCaption.setText (hasSource ? caption : juce::String(), juce::dontSendNotification);
    artworkCaption.setColour (juce::Label::textColourId,
                              artworkNoteIsError ? problemColour.brighter (0.3f)
                                                 : findColour (juce::Label::textColourId).withAlpha (0.8f));

    detailsLabel.setVisible (hasSource);
    fingerprintLabel.setVisible (hasSource);
    artworkCaption.setVisible (hasSource);
    artworkView->setVisible (hasSource);

    openButton.setVisible (stage != ImportStage::inspecting);
    confirmButton.setVisible (isConfirm);
    replaceButton.setVisible (isConfirm);
    fallbackButton.setVisible (isConfirm && artwork.getSelectedOrigin() != model::ArtworkOrigin::fallback);
    embeddedButton.setVisible (isConfirm && artwork.hasEmbedded()
                               && artwork.getSelectedOrigin() != model::ArtworkOrigin::embedded);
    changeArtworkButton.setVisible (isPrepare);
    backButton.setVisible (true);
    previewButton.setVisible (stage == ImportStage::chooseSource || isPrepare);

    resized();
    repaint();
}

//==============================================================================
void ImportScreen::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void ImportScreen::resized()
{
    auto area = getLocalBounds().reduced (32);

    titleLabel.setBounds (area.removeFromTop (44));
    area.removeFromTop (8);

    auto navigation = area.removeFromBottom (36);
    area.removeFromBottom (16);

    backButton.setBounds (navigation.removeFromLeft (180));
    navigation.removeFromLeft (12);

    if (previewButton.isVisible())
        previewButton.setBounds (navigation.removeFromLeft (200));

    openButton.setBounds (navigation.removeFromRight (220));

    if (stage != ImportStage::confirmArtwork && stage != ImportStage::prepare)
    {
        messageLabel.setBounds (area.removeFromTop (juce::jmin (area.getHeight(), 220)));
        return;
    }

    // Artwork on the left, the source description and choices on the right.
    const auto artSide = juce::jmin (area.getHeight() - 70, area.getWidth() / 2 - 16);
    auto left = area.removeFromLeft (juce::jmax (120, artSide));
    area.removeFromLeft (24);

    artworkView->setBounds (left.removeFromTop (left.getWidth()));
    left.removeFromTop (8);
    artworkCaption.setBounds (left);

    messageLabel.setBounds (area.removeFromTop (60));
    area.removeFromTop (8);

    // Stage choices stacked at the bottom of the right column.
    constexpr int buttonHeight = 34;
    constexpr int buttonGap = 8;
    juce::Array<juce::TextButton*> choices;

    for (auto* button : { &confirmButton, &replaceButton, &embeddedButton, &fallbackButton, &changeArtworkButton })
        if (button->isVisible())
            choices.add (button);

    auto buttons = area.removeFromBottom (choices.size() * (buttonHeight + buttonGap));
    area.removeFromBottom (4);

    for (auto* button : choices)
    {
        button->setBounds (buttons.removeFromTop (buttonHeight).withWidth (juce::jmin (260, buttons.getWidth())));
        buttons.removeFromTop (buttonGap);
    }

    fingerprintLabel.setBounds (area.removeFromBottom (stage == ImportStage::prepare ? 64 : 48));
    area.removeFromBottom (8);
    detailsLabel.setBounds (area);
}

} // namespace snds::ui
