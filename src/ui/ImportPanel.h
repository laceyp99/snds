#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "app/ErrorReport.h"
#include "app/SongSession.h"
#include "ui/ArtworkConfirmationPanel.h"
#include "ui/ErrorBanner.h"

#include <functional>
#include <vector>

namespace snds
{
/** One recent song. It opens from the cache whenever the cache entry is valid, whether or not the
    original file is still where it was. */
struct LibraryItem
{
    enum class State
    {
        ready,          // prepared, original found
        originalMoved,  // prepared, original not at its last known place: opens, Locate re-links
        needsPreparing, // no valid prepared copy, but the original exists: opening prepares it
        missing         // no valid prepared copy and no original: Locate or Remove
    };

    juce::String key;
    juce::String title;
    juce::String subtitle;
    juce::String sourcePath; // where the original was last seen
    State state = State::ready;

    bool opensFromCache() const noexcept { return state == State::ready || state == State::originalMoved; }
    bool canOpen() const noexcept { return state != State::missing; }
    bool canLocate() const noexcept { return state == State::originalMoved || state == State::missing; }
};

/** The import area: the Import and Cancel buttons, the status of the current operation with
    progress, the artwork confirmation step, failure actions, and the list of songs already
    prepared on this computer. It only displays state; MainComponent wires it to the session.
*/
class ImportPanel final : public juce::Component, private juce::Timer
{
public:
    ImportPanel();
    ~ImportPanel() override;

    void setStatus (const SessionStatus& status);
    void setLibrary (std::vector<LibraryItem> items);

    /** Importing needs a ready worker and no running operation. */
    void setImportAllowed (bool allowed);

    /** Opening a prepared song needs no worker, only that no operation is running. */
    void setOpenAllowed (bool allowed);
    void setForceCpu (bool forceCpu);

    /** The height this panel needs for what it shows now: the header row, the status or failure, and (unless
        the artwork question or failure details are open) the recent list with room for at least five rows. */
    int getPreferredHeight() const;
    std::function<void()> onPreferredHeightChanged;

    std::function<void()> onImport, onCancel, onPlay;

    /** A recovery action of a failure report was pressed (retry, choose another file, ...). */
    std::function<void (RecoveryKind)> onRecovery;
    std::function<void (ArtworkChoice)> onArtworkChosen;

    /** The buttons and double-click of the recent list; MainComponent decides what each means. */
    std::function<void (const LibraryItem&)> onOpenItem, onLocateItem, onRemoveItem;
    std::function<void (bool)> onForceCpuChanged;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class LibraryModel;

    void refreshControls();
    void timerCallback() override;
    void updateStageText();

    SessionStatus status;
    bool importAllowed = false;
    bool openAllowed = true;
    double busySinceMs = 0.0; // when the current operation began, for the elapsed time
    double progressValue = -1.0;
    int lastPreferredHeight = 0;

    juce::TextButton importButton { "Import song..." }, cancelButton { "Cancel" }, openButton { "Open" },
        locateButton { "Locate..." }, removeButton { "Remove" };
    juce::TextButton playButton { "Play" }; // shown over Cancel once a song was just prepared
    juce::ToggleButton forceCpuToggle { "Use the CPU only" };
    juce::Label headline, detail, guidance, stageLabel, libraryLabel;
    juce::ProgressBar progressBar { progressValue };
    ErrorBanner errorBanner; // a failed import, preparation or cache lookup, in the shared form
    ArtworkConfirmationPanel artworkPanel;
    std::unique_ptr<LibraryModel> libraryModel;
    juce::ListBox library;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ImportPanel)
};
} // namespace snds
