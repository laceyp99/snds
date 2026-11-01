#include <juce_core/juce_core.h>

#include "TestHelpers.h"
#include "app/Settings.h"

namespace snds
{
namespace
{
// The shape of the settings.json an earlier SNDS prototype left in %LOCALAPPDATA%\snds.
constexpr auto oldPrototypeSettings =
    R"({"schema": 1, "force_cpu": true, "recent": [{"path": "D:\\Music\\song.mp3", "fingerprint": "d1ed8ccf"}]})";

class SettingsTests final : public juce::UnitTest
{
public:
    SettingsTests()
        : juce::UnitTest ("Settings", "snds")
    {
    }

    void runTest() override
    {
        beginTest ("A missing file gives defaults and writes nothing");
        {
            const test::ScopedTempDirectory temp;
            Settings settings (temp.child ("settings.json"));
            const auto result = settings.load();

            expect (result.status == Settings::LoadStatus::missing);
            expect (result.warnings.isEmpty());
            expect (! settings.getForceCpu());
            expect (! settings.getFile().exists());
        }

        beginTest ("Saved settings are an SNDS file that loads back");
        {
            const test::ScopedTempDirectory temp;
            const auto file = temp.child ("settings.json");

            Settings settings (file);
            settings.load();
            settings.setForceCpu (true);
            expect (settings.save().wasOk());
            expect (settings.getLastBackup() == juce::File());

            const auto json = juce::JSON::parse (file);
            expectEquals (json["format"].toString(), juce::String (Settings::formatName));
            expectEquals (static_cast<int> (json["version"]), Settings::formatVersion);
            expect (static_cast<bool> (json["forceCpu"]));

            Settings reloaded (file);
            const auto result = reloaded.load();
            expect (result.status == Settings::LoadStatus::loaded);
            expect (result.warnings.isEmpty());
            expect (reloaded.getForceCpu());
        }

        beginTest ("Old prototype settings are foreign: defaults now, one backup on the first save");
        {
            const test::ScopedTempDirectory temp;
            const auto file = temp.child ("settings.json");
            file.replaceWithText (oldPrototypeSettings);

            Settings settings (file);
            const auto result = settings.load();
            expect (result.status == Settings::LoadStatus::foreign);
            expectEquals (result.warnings.size(), 1);
            expect (! settings.getForceCpu(), "foreign keys must not be adopted");

            expect (settings.save().wasOk());
            const auto backup = temp.child ("settings.json.bak");
            expect (settings.getLastBackup() == backup);
            expectEquals (backup.loadFileAsString(), juce::String (oldPrototypeSettings));
            expectEquals (juce::JSON::parse (file)["format"].toString(), juce::String (Settings::formatName));

            expect (settings.save().wasOk());
            expect (settings.getLastBackup() == juce::File());
            expectEquals (temp.get().getNumberOfChildFiles (juce::File::findFiles), 2);

            Settings reloaded (file);
            expect (reloaded.load().status == Settings::LoadStatus::loaded);
        }

        beginTest ("Unparseable settings are backed up before being replaced");
        {
            const test::ScopedTempDirectory temp;
            const auto file = temp.child ("settings.json");
            file.replaceWithText ("{not json");

            Settings settings (file);
            expect (settings.load().status == Settings::LoadStatus::invalid);
            expect (settings.save().wasOk());
            expectEquals (temp.child ("settings.json.bak").loadFileAsString(), juce::String ("{not json"));
        }

        beginTest ("An existing backup is never overwritten");
        {
            const test::ScopedTempDirectory temp;
            const auto file = temp.child ("settings.json");
            const auto olderBackup = temp.child ("settings.json.bak");
            olderBackup.replaceWithText ("older backup");
            file.replaceWithText ("[1, 2]");

            Settings settings (file);
            expect (settings.load().status == Settings::LoadStatus::invalid);
            expect (settings.save().wasOk());

            expectEquals (olderBackup.loadFileAsString(), juce::String ("older backup"));
            expect (settings.getLastBackup().existsAsFile() && settings.getLastBackup() != olderBackup);
            expectEquals (settings.getLastBackup().loadFileAsString(), juce::String ("[1, 2]"));
        }

        beginTest ("Settings from a newer SNDS are backed up, not silently replaced");
        {
            const test::ScopedTempDirectory temp;
            const auto file = temp.child ("settings.json");
            file.replaceWithText (R"({"format": "snds-settings", "version": 99, "forceCpu": true})");

            Settings settings (file);
            expect (settings.load().status == Settings::LoadStatus::newerVersion);
            expect (! settings.getForceCpu());
            expect (settings.save().wasOk());
            expect (settings.getLastBackup().existsAsFile());
        }

        beginTest ("Invalid values fall back to defaults and unknown keys survive a save");
        {
            const test::ScopedTempDirectory temp;
            const auto file = temp.child ("settings.json");
            file.replaceWithText (
                R"({"format": "snds-settings", "version": 1, "forceCpu": "yes", "future": {"a": 1}})");

            Settings settings (file);
            const auto result = settings.load();
            expect (result.status == Settings::LoadStatus::loaded);
            expectEquals (result.warnings.size(), 1);
            expect (! settings.getForceCpu());

            expect (settings.save().wasOk());
            expect (settings.getLastBackup() == juce::File());
            expectEquals (static_cast<int> (juce::JSON::parse (file)["future"]["a"]), 1);
        }

        beginTest ("Saving creates the data folder");
        {
            const test::ScopedTempDirectory temp;
            Settings settings (temp.child ("new\\home\\settings.json"));
            settings.load();
            expect (settings.save().wasOk());
            expect (settings.getFile().existsAsFile());
        }
    }
};

SettingsTests settingsTests;
} // namespace
} // namespace snds
