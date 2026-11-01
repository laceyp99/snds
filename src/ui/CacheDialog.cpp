#include "ui/CacheDialog.h"

#include "ui/SongPanel.h"

namespace snds
{
namespace
{
const juce::Colour warningColour (0xffd29922);
const juce::Colour errorColour (0xfff85149);

juce::String titleOf (const CacheEntry& entry)
{
    return entry.isPlayable() ? entry.song->source.fileName : "Unusable entry " + entry.key.substring (0, 8);
}

juce::String detailOf (const CacheEntry& entry)
{
    const auto size = formatBytes (entry.sizeBytes);

    if (entry.isPlayable())
    {
        const auto& song = entry.song.value();
        return formatDuration (song.source.durationSeconds) + "  -  " + size + "  -  prepared "
             + song.preparedAt.upToFirstOccurrenceOf ("T", false, false);
    }

    const auto kind = entry.status == SongLoadResult::Status::incompatible ? "Made by another version" : "Damaged";
    return kind + juce::String (": ") + entry.reason + "  -  " + size;
}
} // namespace

class CacheComponent::Model final : public juce::ListBoxModel
{
public:
    explicit Model (CacheComponent& owner)
        : view (owner)
    {
    }

    int getNumRows() override { return static_cast<int> (view.overview.entries.size()); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (row < 0 || row >= getNumRows())
            return;

        if (selected)
            g.fillAll (juce::Colours::white.withAlpha (0.12f));

        const auto& entry = view.overview.entries[static_cast<size_t> (row)];
        const auto text = view.findColour (juce::Label::textColourId);
        const auto open = entry.key == view.openKey && entry.isPlayable();

        g.setColour (text);
        g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        g.drawText (titleOf (entry) + (open ? "  (open)" : ""),
                    8,
                    0,
                    width / 3,
                    height,
                    juce::Justification::centredLeft,
                    true);
        g.setColour (entry.isPlayable() ? text.withAlpha (0.65f) : warningColour);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (detailOf (entry), width / 3, 0, width * 2 / 3 - 8, height, juce::Justification::centredRight, true);
    }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
    {
        if (row >= 0 && row < getNumRows())
            view.openButton.triggerClick();
    }

    void selectedRowsChanged (int) override { view.refreshControls(); }

private:
    CacheComponent& view;
};

CacheComponent::CacheComponent()
    : model (std::make_unique<Model> (*this)),
      list ("cache", model.get())
{
    summary.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    note.setFont (juce::FontOptions (13.0f));
    note.setColour (juce::Label::textColourId, findColour (juce::Label::textColourId).withAlpha (0.75f));
    message.setFont (juce::FontOptions (13.0f));
    list.setRowHeight (30);
    list.setOutlineThickness (1);

    confirm = [] (const juce::String& title,
                  const juce::String& text,
                  const juce::String& yesLabel,
                  std::function<void()> onYes)
    {
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::WarningIcon)
                                          .withTitle (title)
                                          .withMessage (text)
                                          .withButton (yesLabel)
                                          .withButton ("Cancel"),
                                      [onYes = std::move (onYes)] (int result)
                                      {
                                          if (result == 1 && onYes)
                                              onYes();
                                      });
    };

    openButton.onClick = [this]
    {
        if (const auto* entry = selected(); entry != nullptr && entry->isPlayable() && onOpen)
            onOpen (entry->key);
    };
    deleteButton.onClick = [this] { askDelete(); };
    verifyButton.onClick = [this]
    {
        if (const auto* entry = selected(); ! locked && entry != nullptr && entry->isPlayable() && onVerify)
            onVerify (entry->key);
    };
    clearButton.onClick = [this] { askClearAll(); };
    closeButton.onClick = [this]
    {
        if (onClose)
            onClose();
    };

    for (auto* child : std::initializer_list<juce::Component*> {
             &summary, &note, &message, &list, &openButton, &deleteButton, &clearButton, &verifyButton, &closeButton })
        addAndMakeVisible (child);

    setSize (720, 460);
    refreshControls();
}

CacheComponent::~CacheComponent() = default;

const CacheEntry* CacheComponent::selected() const
{
    const auto row = list.getSelectedRow();
    return row >= 0 && row < static_cast<int> (overview.entries.size()) ? &overview.entries[static_cast<size_t> (row)]
                                                                        : nullptr;
}

void CacheComponent::setOverview (CacheOverview newOverview)
{
    const auto* before = selected();
    const auto selectedKey = before != nullptr ? before->key : juce::String();

    overview = std::move (newOverview);
    list.updateContent();
    list.deselectAllRows();

    for (size_t i = 0; i < overview.entries.size() && selectedKey.isNotEmpty(); ++i)
        if (overview.entries[i].key == selectedKey)
            list.selectRow (static_cast<int> (i), true);

    list.repaint();
    refreshControls();
}

void CacheComponent::setLocked (bool shouldLock, const juce::String& reason)
{
    locked = shouldLock;
    lockReason = reason;
    refreshControls();
}

void CacheComponent::setOpenKey (const juce::String& key)
{
    openKey = key;
    list.repaint();
}

void CacheComponent::showMessage (const juce::String& text, bool isError)
{
    message.setText (text, juce::dontSendNotification);
    message.setTooltip (text);
    message.setColour (juce::Label::textColourId, isError ? errorColour : findColour (juce::Label::textColourId));
}

void CacheComponent::refreshControls()
{
    summary.setText (juce::String (overview.playableCount())
                         + (overview.playableCount() == 1 ? " prepared song" : " prepared songs") + " use "
                         + formatBytes (overview.usage.cacheBytes)
                         + " in total. Temporary files: " + formatBytes (overview.usage.jobBytes) + ".",
                     juce::dontSendNotification);

    auto text = lockReason;

    if (! locked)
        text = "SNDS never deletes songs on its own. Deleting one only means it is prepared again the next time"
               " you import it; your original files are never touched."
             + (overview.unusableCount() > 0
                    ? juce::String (" Unusable entries (" + juce::String (overview.unusableCount())
                                    + ") can be deleted; they are replaced when you import the song again.")
                    : juce::String());

    note.setText (text, juce::dontSendNotification);

    const auto* entry = selected();
    openButton.setEnabled (! locked && entry != nullptr && entry->isPlayable());
    verifyButton.setEnabled (! locked && entry != nullptr && entry->isPlayable());
    deleteButton.setEnabled (! locked && entry != nullptr);
    clearButton.setEnabled (! locked && ! overview.entries.empty());
}

void CacheComponent::askDelete()
{
    const auto* entry = selected();

    if (locked || entry == nullptr || ! onDelete)
        return;

    const auto key = entry->key;

    // An unusable entry holds nothing worth keeping: no question.
    if (! entry->isPlayable())
        return onDelete (key);

    confirm ("Delete this song?",
             "Delete \"" + titleOf (*entry) + "\" (" + formatBytes (entry->sizeBytes)
                 + ")?\n\nIt is prepared again the next time you import it, which can take a few minutes."
                   " Your original file is not touched."
                 + (key == openKey ? juce::String ("\n\nIt is open now and will be closed first.") : juce::String()),
             "Delete",
             [safe = juce::Component::SafePointer<CacheComponent> (this), key]
             {
                 if (safe != nullptr && safe->onDelete)
                     safe->onDelete (key);
             });
}

void CacheComponent::askClearAll()
{
    if (locked || overview.entries.empty() || ! onClearAll)
        return;

    const auto count = static_cast<int> (overview.entries.size());
    confirm ("Clear the cache?",
             "Delete all " + juce::String (count) + (count == 1 ? " entry" : " entries") + " ("
                 + formatBytes (overview.usage.cacheBytes)
                 + ")?\n\nEvery song is prepared again the next time you import it, which can take a few minutes each."
                   " Your original files are not touched."
                 + (openKey.isNotEmpty() ? juce::String ("\n\nThe song that is open will be closed first.")
                                         : juce::String()),
             "Clear cache",
             [safe = juce::Component::SafePointer<CacheComponent> (this)]
             {
                 if (safe != nullptr && safe->onClearAll)
                     safe->onClearAll();
             });
}

void CacheComponent::resized()
{
    auto area = getLocalBounds().reduced (12);
    summary.setBounds (area.removeFromTop (24));
    note.setBounds (area.removeFromTop (40));
    area.removeFromTop (4);

    auto bottom = area.removeFromBottom (30);
    closeButton.setBounds (bottom.removeFromRight (90));
    bottom.removeFromRight (8);
    clearButton.setBounds (bottom.removeFromRight (130));
    bottom.removeFromRight (8);
    deleteButton.setBounds (bottom.removeFromRight (90));
    bottom.removeFromRight (8);
    verifyButton.setBounds (bottom.removeFromRight (90));
    bottom.removeFromRight (8);
    openButton.setBounds (bottom.removeFromRight (90));
    area.removeFromBottom (6);

    message.setBounds (area.removeFromBottom (48));
    area.removeFromBottom (4);
    list.setBounds (area);
}

//==============================================================================
CacheWindow::CacheWindow()
    : DialogWindow ("Song cache",
                    juce::LookAndFeel::getDefaultLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId),
                    true)
{
    content = new CacheComponent();
    // Closing from a button inside the window: delete after that click has been handled.
    content->onClose = [safe = juce::Component::SafePointer<CacheWindow> (this)]
    {
        juce::MessageManager::callAsync (
            [safe]
            {
                if (safe != nullptr)
                    delete safe.getComponent();
            });
    };
    setContentOwned (content, true);
    setUsingNativeTitleBar (true);
    setResizable (true, false);
}
} // namespace snds
