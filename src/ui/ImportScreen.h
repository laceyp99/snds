#pragma once

#include "app/AppPaths.h"
#include "app/Logging.h"
#include "app/SongImport.h"
#include "app/SourceFingerprint.h"
#include "model/AppState.h"
#include "model/SongSource.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>

namespace snds::ui
{

/**
    Song import: choose a WAV/MP3 file, see why it was rejected or confirm its artwork, then reach
    the preparation step, which for now shows the confirmed source, artwork, and fingerprint.

    Threads: inspection, artwork extraction and decoding, and replacement-image loading run on a
    private one-thread pool. Fingerprinting starts as soon as a file is accepted and runs on a
    BackgroundFingerprinter, so hashing overlaps artwork confirmation. Results return through
    MessageManager::callAsync and are dropped if another file has been opened since.
*/
class ImportScreen final : public juce::Component, private juce::Timer
{
public:
    ImportScreen (const AppPaths& paths, logging::LogService& log);
    ~ImportScreen() override;

    /** Imports a file as if the user had chosen it (also used by the --open command-line flag). */
    void openSource (const juce::File& file);

    model::ImportStage getStage() const noexcept { return stage; }

    std::function<void()> onBackToReadiness;
    std::function<void()> onPreviewPlayer;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    class ArtworkView;
    struct Inspection;

    void chooseSourceFile();
    void chooseReplacementArtwork();
    void applyInspection (int generationOfResult, const Inspection& inspection);
    void applyFingerprint (int generationOfResult, const FingerprintOutcome& outcome);
    void setStage (model::ImportStage newStage);
    void refreshContent();
    juce::String describeSource() const;
    juce::String describeFingerprint (bool fullHash) const;
    void timerCallback() override;

    logging::LogService& log;
    std::shared_ptr<FingerprintMemo> memo;
    BackgroundFingerprinter fingerprinter;

    model::ImportStage stage = model::ImportStage::chooseSource;
    int generation = 0;
    juce::File currentFile;
    std::optional<model::SourceInfo> source;
    model::ArtworkChoice artwork;
    juce::String artworkNote;
    bool artworkNoteIsError = false;
    std::optional<FingerprintOutcome> fingerprint;
    ImportError problem;
    std::unique_ptr<juce::FileChooser> chooser;

    juce::Label titleLabel;
    juce::Label messageLabel;
    juce::Label detailsLabel;
    juce::Label fingerprintLabel;
    juce::Label artworkCaption;
    std::unique_ptr<ArtworkView> artworkView;
    juce::TextButton openButton { "Open song..." };
    juce::TextButton confirmButton { "Use this artwork" };
    juce::TextButton replaceButton { "Replace..." };
    juce::TextButton fallbackButton { "Use generated artwork" };
    juce::TextButton embeddedButton { "Use embedded artwork" };
    juce::TextButton changeArtworkButton { "Change artwork" };
    juce::TextButton backButton { "Back to readiness" };
    juce::TextButton previewButton { "Preview player layout" };

    // Declared last so it is destroyed first: waits for in-flight inspection before members go away.
    juce::ThreadPool workPool { juce::ThreadPoolOptions {}.withThreadName ("SNDS import").withNumberOfThreads (1) };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ImportScreen)
};

} // namespace snds::ui
