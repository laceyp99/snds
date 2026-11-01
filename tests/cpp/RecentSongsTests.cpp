#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "FakeAudioDevice.h"
#include "SongFixtures.h"
#include "TestHelpers.h"
#include "app/CacheStore.h"
#include "app/DataPaths.h"
#include "app/RecentSongs.h"
#include "app/Settings.h"
#include "ui/ImportPanel.h"
#include "ui/RecentList.h"

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

class RecentSongsTests final : public juce::UnitTest
{
public:
    RecentSongsTests()
        : juce::UnitTest ("RecentSongs", "snds")
    {
    }

    void runTest() override
    {
        testList();
        testSettings();
        testRows();
        testPanel();
        testPanelHeight();
    }

private:
    void testList()
    {
        beginTest ("RecentSongs: newest first, one entry per song, capped");
        std::vector<RecentSong> list;

        for (int i = 0; i < 25; ++i)
            list = recent::withRecorded (std::move (list), { "k" + juce::String (i), "t", "p" });

        expectEquals (static_cast<int> (list.size()), static_cast<int> (recent::maxEntries));
        expectEquals (list.front().key, juce::String ("k24"));
        expect (! recent::find (list, "k0").has_value(), "the oldest fell off");

        list = recent::withRecorded (std::move (list), { "k10", "renamed", "elsewhere" });
        expectEquals (list.front().key, juce::String ("k10"));
        expectEquals (list.front().sourcePath, juce::String ("elsewhere"));
        expectEquals (static_cast<int> (list.size()), static_cast<int> (recent::maxEntries), "no duplicate");

        list = recent::without (std::move (list), "k10");
        expect (! recent::find (list, "k10").has_value());
        expectEquals (static_cast<int> (list.size()), static_cast<int> (recent::maxEntries) - 1);
    }

    void testSettings()
    {
        beginTest ("RecentSongs: settings keep only references, survive a reload, and drop unusable entries");
        const ScopedTempDirectory temp;
        const auto file = temp.child ("settings.json");

        {
            Settings settings (file);
            settings.load();
            settings.setForceCpu (true);
            settings.setRecentSongs ({ { "abc-123", "a.wav", "C:\\Music\\a.wav" }, { "def-456", "b.mp3", "" } });
            expect (settings.save().wasOk());
        }

        const auto text = file.loadFileAsString();
        expect (! text.containsIgnoreCase ("volume") && ! text.containsIgnoreCase ("mute"), "no mix state is stored");

        Settings reloaded (file);
        expect (reloaded.load().status == Settings::LoadStatus::loaded);
        const auto songs = reloaded.getRecentSongs();
        expectEquals (static_cast<int> (songs.size()), 2);
        expectEquals (songs[0].key, juce::String ("abc-123"));
        expectEquals (songs[0].sourcePath, juce::String ("C:\\Music\\a.wav"));
        expect (reloaded.getForceCpu(), "other settings are kept");

        // A hand-edited file: a path-like key and a missing key are not references to the cache.
        file.replaceWithText (R"({"format":"snds-settings","version":1,"recentSongs":[
            {"key":"..\\..\\Windows","title":"x","path":"y"},
            {"title":"no key"},
            {"key":"good-1","title":"ok","path":"p"}]})");
        Settings edited (file);
        edited.load();
        const auto kept = edited.getRecentSongs();
        expectEquals (static_cast<int> (kept.size()), 1);
        expectEquals (kept[0].key, juce::String ("good-1"));

        file.replaceWithText (R"({"format":"snds-settings","version":1,"recentSongs":"nope"})");
        Settings wrongType (file);
        expect (! wrongType.load().warnings.isEmpty());
        expect (wrongType.getRecentSongs().empty());
    }

    void testRows()
    {
        beginTest ("RecentSongs: a prepared song opens from the cache even when its original moved");
        const ScopedTempDirectory temp;
        CacheStore cache (DataPaths { temp.child ("home") });

        SongFixture first, second;
        second.sha256 = juce::String::repeatedString ("cd", 32);
        first.write (cache.cacheDirectory());
        second.write (cache.cacheDirectory());

        const auto here = temp.child ("music/here.wav");
        here.getParentDirectory().createDirectory();
        here.replaceWithText ("x");
        const auto gone = temp.child ("moved/gone.wav");

        const std::vector<RecentSong> recents { { first.key(), "a.wav", "" },
                                                { second.key(), "b.wav", "" },
                                                { "never-prepared-1", "c.wav", "" },
                                                { "never-prepared-2", "d.wav", "" } };
        const auto rows = buildRecentItems (recents, { here, gone, here, gone }, cache.listPlayableEntries());

        expectEquals (static_cast<int> (rows.size()), 4);

        if (rows.size() != 4)
            return;

        expect (rows[0].state == LibraryItem::State::ready, "prepared, original found");
        expect (rows[0].opensFromCache() && ! rows[0].canLocate());
        expect (rows[1].state == LibraryItem::State::originalMoved, "prepared, original gone");
        expect (rows[1].opensFromCache() && rows[1].canOpen() && rows[1].canLocate());
        expect (rows[2].state == LibraryItem::State::needsPreparing, "not prepared, original exists");
        expect (! rows[2].opensFromCache() && rows[2].canOpen() && ! rows[2].canLocate());
        expect (rows[3].state == LibraryItem::State::missing, "not prepared, no original");
        expect (! rows[3].canOpen() && rows[3].canLocate());
        expectEquals (rows[2].sourcePath, here.getFullPathName());
        expectEquals (rows[3].title, juce::String ("d.wav"));

        // Sizes read as a person writes them ("8 KB"), never with every digit of the division.
        expect (rows[0].subtitle.endsWith (" KB") && ! rows[0].subtitle.contains ("."), rows[0].subtitle);
    }

    void testPanel()
    {
        beginTest ("RecentSongs: the list offers Open, Locate and Remove only where they apply");
        ImportPanel panel;
        panel.setSize (900, 600);

        LibraryItem moved { "k1", "moved.wav", "prepared", "C:\\x\\moved.wav", LibraryItem::State::originalMoved };
        LibraryItem missing { "k2", "lost.wav", "gone", "C:\\x\\lost.wav", LibraryItem::State::missing };
        LibraryItem ready { "k3", "fine.wav", "ok", "C:\\x\\fine.wav", LibraryItem::State::ready };
        panel.setLibrary ({ moved, missing, ready });

        std::vector<juce::String> located, removed, opened;
        panel.onLocateItem = [&] (const LibraryItem& item) { located.push_back (item.key); };
        panel.onRemoveItem = [&] (const LibraryItem& item) { removed.push_back (item.key); };
        panel.onOpenItem = [&] (const LibraryItem& item) { opened.push_back (item.key); };

        auto* list = findList (panel);
        auto* open = findButton (panel, "Open");
        auto* locate = findButton (panel, "Locate...");
        auto* remove = findButton (panel, "Remove");
        expect (list != nullptr && open != nullptr && locate != nullptr && remove != nullptr);

        if (list == nullptr || open == nullptr || locate == nullptr || remove == nullptr)
            return;

        list->selectRow (1);
        expect (! open->isEnabled(), "a missing song cannot be opened");
        expect (locate->isEnabled() && remove->isEnabled());
        locate->triggerClick();
        remove->triggerClick();
        MessageLoop::pump (40);
        expect (located.size() == 1 && located[0] == "k2");
        expect (removed.size() == 1 && removed[0] == "k2");

        list->selectRow (2);
        expect (open->isEnabled() && ! locate->isEnabled(), "a found original needs no Locate");
        open->triggerClick();
        MessageLoop::pump (40);
        expect (opened.size() == 1 && opened[0] == "k3");

        list->selectRow (0);
        expect (open->isEnabled() && locate->isEnabled());

        // The list is rebuilt after each open; the same song stays selected.
        panel.setLibrary ({ ready, moved });
        expect (list->getSelectedRow() == 1, "selection follows the song, not the row number");
    }

    void testPanelHeight()
    {
        beginTest ("RecentSongs: the panel asks for room for five rows, in every state, and gets them");
        ImportPanel panel;
        int asked = 0;
        panel.onPreferredHeightChanged = [&] { ++asked; };

        std::vector<LibraryItem> songs;

        for (int i = 0; i < 6; ++i)
            songs.push_back ({ "k" + juce::String (i),
                               "song " + juce::String (i),
                               "ok",
                               "C:\\Music\\a.wav",
                               LibraryItem::State::ready });

        panel.setLibrary (songs);
        const auto idle = panel.getPreferredHeight();
        panel.setSize (900, idle);
        auto* list = findList (panel);
        expect (list != nullptr);

        if (list == nullptr)
            return;

        expect (list->getHeight() >= 5 * 30, "five rows with nothing open: " + juce::String (list->getHeight()));

        // A song is open: a headline and a detail line take space above the list, and the panel says so.
        asked = 0;
        SessionStatus open;
        open.headline = "Ready: song";
        open.detail = "Opened from your library; it was already prepared, so nothing was separated.";
        panel.setStatus (open);
        expect (asked > 0, "the owner is told the panel needs more room");
        expect (panel.getPreferredHeight() >= idle + 36, "the detail line is paid for");
        panel.setSize (900, panel.getPreferredHeight());
        expect (list->getHeight() >= 5 * 30, "five rows with a song open: " + juce::String (list->getHeight()));

        // The artwork question has no list, so it asks only for its own height.
        SessionStatus artwork;
        artwork.state = SessionState::awaitingArtwork;
        artwork.songName = "song.wav";
        panel.setStatus (artwork);
        expect (! list->isVisible());
        expect (panel.getPreferredHeight() < idle + 36 + 5 * 30 + 200);
    }
};

RecentSongsTests recentSongsTests;
} // namespace
} // namespace snds
