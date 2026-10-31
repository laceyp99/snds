#include "ui/ImportPanel.h"

#include "app/ProgressText.h"

#include <cmath>
#include <utility>

namespace snds
{
namespace
{
constexpr int padding = 16;
constexpr int rowHeight = 32;
constexpr int listRowHeight = 30;
constexpr int minListRows = 5;

} // namespace

class ImportPanel::LibraryModel final : public juce::ListBoxModel
{
public:
    explicit LibraryModel (ImportPanel& owner)
        : panel (owner)
    {
    }

    int getNumRows() override { return static_cast<int> (items.size()); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (row < 0 || row >= getNumRows())
            return;

        if (selected)
            g.fillAll (juce::Colours::white.withAlpha (0.12f));

        const auto& item = items[static_cast<size_t> (row)];
        const auto text = panel.findColour (juce::Label::textColourId);
        g.setColour (text);
        g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        g.drawText (item.title, 8, 0, width / 2, height, juce::Justification::centredLeft, true);
        g.setColour (item.state == LibraryItem::State::ready ? text.withAlpha (0.65f) : juce::Colour (0xffd29922));
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (item.subtitle, width / 2, 0, width / 2 - 8, height, juce::Justification::centredRight, true);
    }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override { open (row); }
    void returnKeyPressed (int row) override { open (row); }
    void selectedRowsChanged (int) override { panel.refreshControls(); }

    void open (int row)
    {
        if (row >= 0 && row < getNumRows() && panel.onOpenItem != nullptr && panel.openAllowed
            && ! panel.status.isBusy() && items[static_cast<size_t> (row)].canOpen())
            panel.onOpenItem (items[static_cast<size_t> (row)]);
    }

    const LibraryItem* selected() const
    {
        const auto row = panel.library.getSelectedRow();
        return row >= 0 && row < static_cast<int> (items.size()) ? &items[static_cast<size_t> (row)] : nullptr;
    }

    std::vector<LibraryItem> items;

private:
    ImportPanel& panel;
};

ImportPanel::ImportPanel()
    : libraryModel (std::make_unique<LibraryModel> (*this)),
      library ("library", libraryModel.get())
{
    headline.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    detail.setFont (juce::FontOptions (14.0f));
    guidance.setFont (juce::FontOptions (14.0f, juce::Font::italic));
    stageLabel.setFont (juce::FontOptions (13.0f));
    libraryLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    libraryLabel.setText ("Recent songs", juce::dontSendNotification);

    for (auto* label : { &detail, &guidance, &stageLabel })
        label->setColour (juce::Label::textColourId, findColour (juce::Label::textColourId).withAlpha (0.75f));

    importButton.onClick = [this]
    {
        if (onImport)
            onImport();
    };
    cancelButton.onClick = [this]
    {
        if (onCancel)
            onCancel();
    };
    errorBanner.onAction = [this] (RecoveryKind kind)
    {
        if (onRecovery)
            onRecovery (kind);
    };
    errorBanner.onPreferredHeightChanged = [this] { refreshControls(); };
    playButton.onClick = [this]
    {
        if (onPlay)
            onPlay();
    };
    openButton.onClick = [this] { libraryModel->open (library.getSelectedRow()); };
    locateButton.onClick = [this]
    {
        if (const auto* item = libraryModel->selected(); item != nullptr && onLocateItem)
            onLocateItem (LibraryItem (*item));
    };
    removeButton.onClick = [this]
    {
        if (const auto* item = libraryModel->selected(); item != nullptr && onRemoveItem)
            onRemoveItem (LibraryItem (*item));
    };
    forceCpuToggle.onClick = [this]
    {
        if (onForceCpuChanged)
            onForceCpuChanged (forceCpuToggle.getToggleState());
    };

    artworkPanel.onChosen = [this] (ArtworkChoice choice)
    {
        if (onArtworkChosen)
            onArtworkChosen (std::move (choice));
    };

    library.setRowHeight (listRowHeight);
    library.setOutlineThickness (1);

    for (auto* child : std::initializer_list<juce::Component*> { &importButton,
                                                                 &cancelButton,
                                                                 &openButton,
                                                                 &locateButton,
                                                                 &removeButton,
                                                                 &forceCpuToggle,
                                                                 &headline,
                                                                 &detail,
                                                                 &guidance,
                                                                 &stageLabel,
                                                                 &libraryLabel,
                                                                 &progressBar,
                                                                 &library })
        addAndMakeVisible (child);

    addChildComponent (playButton);
    addChildComponent (errorBanner);
    addChildComponent (artworkPanel);
    refreshControls();
}

ImportPanel::~ImportPanel()
{
    stopTimer();
}

void ImportPanel::setImportAllowed (bool allowed)
{
    importAllowed = allowed;
    refreshControls();
}

void ImportPanel::setOpenAllowed (bool allowed)
{
    openAllowed = allowed;
    refreshControls();
}

void ImportPanel::setForceCpu (bool forceCpu)
{
    forceCpuToggle.setToggleState (forceCpu, juce::dontSendNotification);
}

void ImportPanel::setLibrary (std::vector<LibraryItem> items)
{
    const auto* before = libraryModel->selected();
    const auto selectedKey = before != nullptr ? before->key : juce::String();
    libraryModel->items = std::move (items);
    library.updateContent();

    // The list reorders as songs are opened; keep the same song selected.
    library.deselectAllRows();

    for (size_t i = 0; i < libraryModel->items.size() && selectedKey.isNotEmpty(); ++i)
        if (libraryModel->items[i].key == selectedKey)
            library.selectRow (static_cast<int> (i), true);

    library.repaint();
    refreshControls();
}

void ImportPanel::setStatus (const SessionStatus& newStatus)
{
    const auto previous = status.state;
    const auto wasBusy = status.isBusy();
    status = newStatus;

    if (! wasBusy && status.isBusy())
        busySinceMs = juce::Time::getMillisecondCounterHiRes();

    if (previous != SessionState::awaitingArtwork && status.state == SessionState::awaitingArtwork)
        artworkPanel.show (status.songName, status.songName, status.detectedArtwork);

    progressValue = status.progress;
    headline.setText (status.headline, juce::dontSendNotification);
    detail.setText (status.detail, juce::dontSendNotification);
    guidance.setText (status.guidance, juce::dontSendNotification);
    errorBanner.setReport (reportFromSession (status));

    updateStageText();
    refreshControls();
}

void ImportPanel::updateStageText()
{
    const auto elapsed = (juce::Time::getMillisecondCounterHiRes() - busySinceMs) / 1000.0;
    stageLabel.setText (describeProgress (status, elapsed), juce::dontSendNotification);
}

void ImportPanel::timerCallback()
{
    updateStageText();
}

int ImportPanel::getPreferredHeight() const
{
    auto height = 2 * padding + rowHeight + 4 + 6;

    if (errorBanner.isVisible())
        height += errorBanner.getPreferredHeight() + 8;
    else
        height += 26 + (detail.getText().isEmpty() ? 0 : 36) + (guidance.getText().isEmpty() ? 0 : 36);

    if (stageLabel.isVisible())
        height += 20 + 16 + 6;

    if (artworkPanel.isVisible())
        return height + artworkPanel.getHeight();

    return height + 28 + (library.isVisible() ? minListRows * listRowHeight + 2 : 0);
}

void ImportPanel::refreshControls()
{
    const bool failed = status.state == SessionState::failed;
    const bool busy = status.isBusy();
    const bool awaiting = status.state == SessionState::awaitingArtwork;

    importButton.setEnabled (importAllowed && ! busy);
    cancelButton.setEnabled (status.canCancel);
    cancelButton.setVisible (busy);
    const auto* selected = libraryModel->selected();
    openButton.setEnabled (openAllowed && ! busy && selected != nullptr && selected->canOpen());
    locateButton.setEnabled (! busy && selected != nullptr && selected->canLocate());
    removeButton.setEnabled (! busy && selected != nullptr);
    forceCpuToggle.setEnabled (! busy);
    progressBar.setVisible (busy && ! awaiting);
    stageLabel.setVisible (busy && ! awaiting);
    artworkPanel.setVisible (awaiting);
    headline.setVisible (! failed);
    detail.setVisible (! failed);
    guidance.setVisible (! failed);
    errorBanner.setVisible (failed);

    const bool ready = status.justPrepared && status.state == SessionState::idle;
    playButton.setVisible (ready);

    if (busy && ! awaiting)
        startTimer (1000);
    else
        stopTimer();
    library.setVisible (! awaiting && ! (failed && errorBanner.areDetailsShown()));
    libraryLabel.setVisible (library.isVisible());
    openButton.setVisible (library.isVisible());
    locateButton.setVisible (library.isVisible());
    removeButton.setVisible (library.isVisible());
    resized();

    if (const auto preferred = getPreferredHeight(); preferred != std::exchange (lastPreferredHeight, preferred))
        if (onPreferredHeightChanged)
            onPreferredHeightChanged();
}

void ImportPanel::paint (juce::Graphics& g)
{
    g.setColour (findColour (juce::ResizableWindow::backgroundColourId).brighter (0.15f));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);
}

void ImportPanel::resized()
{
    auto bounds = getLocalBounds().reduced (padding);

    auto top = bounds.removeFromTop (rowHeight + 4);
    importButton.setBounds (top.removeFromLeft (160).withHeight (rowHeight));
    top.removeFromLeft (8);
    cancelButton.setBounds (top.removeFromLeft (100).withHeight (rowHeight));
    playButton.setBounds (cancelButton.getBounds());
    forceCpuToggle.setBounds (top.removeFromRight (180).withHeight (rowHeight));
    bounds.removeFromTop (6);

    if (errorBanner.isVisible())
    {
        errorBanner.setBounds (bounds.removeFromTop (errorBanner.getPreferredHeight()));
        bounds.removeFromTop (8);
    }
    else
    {
        headline.setBounds (bounds.removeFromTop (26));
        detail.setBounds (bounds.removeFromTop (detail.getText().isEmpty() ? 0 : 36));
        guidance.setBounds (bounds.removeFromTop (guidance.getText().isEmpty() ? 0 : 36));
    }

    if (stageLabel.isVisible())
    {
        stageLabel.setBounds (bounds.removeFromTop (20));
        progressBar.setBounds (bounds.removeFromTop (16));
        bounds.removeFromTop (6);
    }

    if (artworkPanel.isVisible())
    {
        artworkPanel.setBounds (
            bounds.removeFromTop (artworkPanel.getHeight()).withWidth (juce::jmin (bounds.getWidth(), 640)));
        return;
    }

    auto libraryHeader = bounds.removeFromTop (28);
    openButton.setBounds (libraryHeader.removeFromRight (80).withHeight (26));
    libraryHeader.removeFromRight (6);
    locateButton.setBounds (libraryHeader.removeFromRight (100).withHeight (26));
    libraryHeader.removeFromRight (6);
    removeButton.setBounds (libraryHeader.removeFromRight (90).withHeight (26));
    libraryLabel.setBounds (libraryHeader);
    library.setBounds (bounds);
}
} // namespace snds
