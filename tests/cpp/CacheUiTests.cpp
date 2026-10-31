#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "FakeAudioDevice.h"
#include "SongFixtures.h"
#include "TestHelpers.h"
#include "app/CacheOverview.h"
#include "app/DataPaths.h"
#include "ui/CacheDialog.h"

namespace snds
{
namespace
{
using namespace snds::test;

juce::TextButton* findButton (juce::Component& parent, const juce::String& text)
{
    for (auto* child : parent.getChildren())
        if (auto* button = dynamic_cast<juce::TextButton*> (child);
            button != nullptr && button->getButtonText() == text)
            return button;

    return nullptr;
}

juce::ListBox* findList (juce::Component& parent)
{
    for (auto* child : parent.getChildren())
        if (auto* list = dynamic_cast<juce::ListBox*> (child))
            return list;

    return nullptr;
}

class CacheUiTests final : public juce::UnitTest
{
public:
    CacheUiTests()
        : juce::UnitTest ("CacheUi", "snds")
    {
    }

    void runTest() override
    {
        testBytes();
        testOverview();
        testComponent();
    }

private:
    void testBytes()
    {
        beginTest ("CacheUi: sizes are shown in the unit a person would pick");
        expectEquals (formatBytes (0), juce::String ("0 B"));
        expectEquals (formatBytes (900), juce::String ("900 B"));
        expectEquals (formatBytes (2048), juce::String ("2 KB"));
        expectEquals (formatBytes (1255), juce::String ("1 KB"), "no stray decimals");
        expectEquals (
            formatBytes (340ll * 1024 * 1024 + 400000), juce::String ("340 MB"), "rounded, not printed in full");
        expectEquals (formatBytes (5 * 1024 * 1024), juce::String ("5.0 MB"));
        expectEquals (formatBytes (340ll * 1024 * 1024), juce::String ("340 MB"));
        expectEquals (formatBytes (3ll * 1024 * 1024 * 1024 / 2), juce::String ("1.5 GB"));
    }

    static SongFixture fixtureWith (const juce::String& pair)
    {
        SongFixture fixture;
        fixture.sha256 = juce::String::repeatedString (pair, 32);
        return fixture;
    }

    void testOverview()
    {
        beginTest ("CacheUi: the overview lists playable, incompatible and damaged entries with their sizes");
        const ScopedTempDirectory temp;
        CacheStore store (DataPaths { temp.child ("home") });

        fixtureWith ("ab").write (store.cacheDirectory(), {}, 3000);
        fixtureWith ("cd").write (store.cacheDirectory(),
                                  [] (juce::DynamicObject& d) { d.setProperty ("cacheSchema", 2); });
        const auto damaged = fixtureWith ("ef").write (store.cacheDirectory());
        damaged.getChildFile ("vocals.wav").deleteFile();

        const auto overview = readCacheOverview (store);
        expectEquals (static_cast<int> (overview.entries.size()), 3);
        expectEquals (overview.playableCount(), 1);
        expectEquals (overview.unusableCount(), 2);
        expect (overview.usage.cacheBytes > 12000, "every entry's files are counted");
        expect (overview.entries.front().isPlayable(), "playable songs come first");
    }

    void testComponent()
    {
        beginTest ("CacheUi: the view shows totals and per-song rows, and asks before deleting a song");
        const ScopedTempDirectory temp;
        CacheStore store (DataPaths { temp.child ("home") });

        const auto good = fixtureWith ("ab");
        good.write (store.cacheDirectory());
        fixtureWith ("cd").write (store.cacheDirectory(),
                                  [] (juce::DynamicObject& d) { d.setProperty ("cacheSchema", 2); });

        CacheComponent view;
        view.setOverview (readCacheOverview (store));
        expect (view.getSummaryText().contains ("1 prepared song use"), view.getSummaryText());
        expect (view.getSummaryText().contains (formatBytes (view.getOverview().usage.cacheBytes)));

        auto* list = findList (view);
        auto* open = findButton (view, "Open");
        auto* remove = findButton (view, "Delete");
        auto* clear = findButton (view, "Clear cache...");
        auto* verify = findButton (view, "Verify");
        expect (list != nullptr && open != nullptr && remove != nullptr && clear != nullptr);

        if (list == nullptr || open == nullptr || remove == nullptr || clear == nullptr || verify == nullptr)
            return;

        expectEquals (list->getListBoxModel()->getNumRows(), 2);

        std::vector<juce::String> opened, deleted;
        std::vector<juce::String> verified;
        int cleared = 0;
        std::vector<juce::String> asked;
        bool answer = false;

        view.onOpen = [&] (const juce::String& key) { opened.push_back (key); };
        view.onDelete = [&] (const juce::String& key) { deleted.push_back (key); };
        view.onVerify = [&] (const juce::String& key) { verified.push_back (key); };
        view.onClearAll = [&] { ++cleared; };
        view.confirm =
            [&] (const juce::String& title, const juce::String&, const juce::String&, std::function<void()> yes)
        {
            asked.push_back (title);

            if (answer)
                yes();
        };

        // A playable song can be opened and is deleted only after a yes.
        list->selectRow (0);
        expect (open->isEnabled() && remove->isEnabled());
        open->triggerClick();
        MessageLoop::pump (40);
        expect (opened.size() == 1 && opened[0] == good.key(), "any playable cached song opens");
        verify->triggerClick();
        MessageLoop::pump (40);
        expect (verified.size() == 1 && verified[0] == good.key());

        remove->triggerClick();
        MessageLoop::pump (40);
        expectEquals (static_cast<int> (asked.size()), 1);
        expect (deleted.empty(), "no answer, no deletion");
        answer = true;
        remove->triggerClick();
        MessageLoop::pump (40);
        expect (deleted.size() == 1 && deleted[0] == good.key());

        // An unusable entry cannot be opened; deleting it needs no question.
        list->selectRow (1);
        expect (! open->isEnabled() && remove->isEnabled());
        expect (! verify->isEnabled());
        asked.clear();
        remove->triggerClick();
        MessageLoop::pump (40);
        expect (asked.empty() && deleted.size() == 2);

        // Clear cache always asks.
        answer = false;
        clear->triggerClick();
        MessageLoop::pump (40);
        expect (asked.size() == 1 && cleared == 0);
        answer = true;
        clear->triggerClick();
        MessageLoop::pump (40);
        expectEquals (cleared, 1);

        // While a song is being prepared or a deletion runs, nothing can be changed.
        view.setLocked (true, "busy");
        expect (! open->isEnabled() && ! remove->isEnabled() && ! clear->isEnabled());
        expect (! verify->isEnabled());
        view.setLocked (false);
        expect (clear->isEnabled());

        // Nothing is ever removed by the view itself: the entries are still on disk.
        expectEquals (static_cast<int> (store.listEntries().size()), 2);
    }
};

CacheUiTests cacheUiTests;
} // namespace
} // namespace snds
