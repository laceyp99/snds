#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "app/CacheOverview.h"

#include <functional>

namespace snds
{
/** The cache view: how much space prepared songs use (total and per song), every entry including
    incompatible and damaged ones, and the actions on them: open a playable song, delete one,
    clear or verify the cache. SNDS never deletes anything by itself; every deletion of a playable song asks
    first. It only displays and reports clicks; MainComponent does the work and calls setOverview
    again.
*/
class CacheComponent final : public juce::Component
{
public:
    CacheComponent();
    ~CacheComponent() override;

    void setOverview (CacheOverview overview);

    /** While true (a song is being prepared or opened, or a deletion/verification is running) nothing can be
        opened or deleted; `reason` is shown in place of the summary note. */
    void setLocked (bool locked, const juce::String& reason = {});

    /** Marks the song that is open in the player; deleting it closes it first. */
    void setOpenKey (const juce::String& key);

    void showMessage (const juce::String& text, bool isError);

    std::function<void (const juce::String& key)> onOpen, onDelete, onVerify;
    std::function<void()> onClearAll, onClose;

    /** Asks the user before a deletion; replaced in tests. The default is a modal-style alert. */
    std::function<void (const juce::String& title,
                        const juce::String& message,
                        const juce::String& yesLabel,
                        std::function<void()> onYes)>
        confirm;

    const CacheOverview& getOverview() const noexcept { return overview; }
    juce::String getSummaryText() const { return summary.getText(); }

    void resized() override;

private:
    class Model;

    const CacheEntry* selected() const;
    void refreshControls();
    void askDelete();
    void askClearAll();

    CacheOverview overview;
    bool locked = false;
    juce::String lockReason, openKey;

    juce::Label summary, note, message;
    std::unique_ptr<Model> model;
    juce::ListBox list;
    juce::TextButton openButton { "Open" }, deleteButton { "Delete" }, clearButton { "Clear cache..." },
        verifyButton { "Verify" }, closeButton { "Close" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CacheComponent)
};

/** The window around a CacheComponent. Deletes itself when closed. */
class CacheWindow final : public juce::DialogWindow
{
public:
    CacheWindow();

    CacheComponent& view() noexcept { return *content; }
    void closeButtonPressed() override { delete this; }

private:
    CacheComponent* content = nullptr; // owned by the window

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CacheWindow)
};
} // namespace snds
