#include <juce_core/juce_core.h>

#include "TestHelpers.h"
#include "app/DataPaths.h"

namespace snds
{
namespace
{
class DataPathsTests final : public juce::UnitTest
{
public:
    DataPathsTests()
        : juce::UnitTest ("DataPaths", "snds")
    {
    }

    void runTest() override
    {
        const juce::File cwd ("C:\\snds-test\\cwd");
        const juce::String localAppData ("C:\\Users\\x\\AppData\\Local");

        beginTest ("SNDS_HOME overrides LOCALAPPDATA");
        expectEquals (home ("D:\\custom\\snds", localAppData, cwd), juce::String ("D:\\custom\\snds"));

        beginTest ("Default is %LOCALAPPDATA%\\snds");
        expectEquals (home ({}, localAppData, cwd), juce::String ("C:\\Users\\x\\AppData\\Local\\snds"));

        beginTest ("Blank or empty-quoted SNDS_HOME falls back to the default");
        for (const auto* blank : { "", "   ", "\"\"" })
            expectEquals (home (blank, localAppData, cwd), juce::String ("C:\\Users\\x\\AppData\\Local\\snds"));

        beginTest ("Quoted, relative SNDS_HOME resolves against the working directory");
        expectEquals (home ("\"data\\..\\home\"", localAppData, cwd), juce::String ("C:\\snds-test\\cwd\\home"));

        beginTest ("No SNDS_HOME and no LOCALAPPDATA gives no paths");
        expect (! DataPaths::resolve ({}, {}, cwd).has_value());
        expect (! DataPaths::resolve ("  ", "  ", cwd).has_value());

        beginTest ("Layout matches the worker");
        {
            const DataPaths paths { juce::File ("C:\\data\\snds") };
            expectEquals (paths.settingsFile().getFullPathName(), juce::String ("C:\\data\\snds\\settings.json"));
            expectEquals (paths.cacheDirectory().getFullPathName(), juce::String ("C:\\data\\snds\\cache"));
            expectEquals (paths.jobsDirectory().getFullPathName(), juce::String ("C:\\data\\snds\\jobs"));
            expectEquals (paths.logsDirectory().getFullPathName(), juce::String ("C:\\data\\snds\\logs"));
            expectEquals (paths.modelFile().getFullPathName(),
                          juce::String ("C:\\data\\snds\\models\\htdemucs\\955717e8-8726e21a.th"));
        }

        beginTest ("Resolution creates nothing on disk");
        {
            const test::ScopedTempDirectory temp;
            const auto paths = DataPaths::resolve (temp.child ("home").getFullPathName(), {}, cwd);
            expect (paths.has_value() && ! paths->home.exists());
            expectEquals (temp.get().getNumberOfChildFiles (juce::File::findFilesAndDirectories), 0);
        }

        beginTest ("fromEnvironment reads this process's environment");
        {
            const auto custom = juce::SystemStats::getEnvironmentVariable ("SNDS_HOME", {});
            const auto expected = DataPaths::resolve (custom,
                                                      juce::SystemStats::getEnvironmentVariable ("LOCALAPPDATA", {}),
                                                      juce::File::getCurrentWorkingDirectory());
            const auto actual = DataPaths::fromEnvironment();
            expect (actual.has_value() && expected.has_value() && actual->home == expected->home);

            // CTest points SNDS_HOME at the build tree so tests never use the real data folder.
            if (custom.isNotEmpty())
                expect (! actual->home.getFullPathName().endsWithIgnoreCase ("AppData\\Local\\snds"));
        }

        beginTest ("App ownership excludes other writers to the same home and releases on close");
        {
            const test::ScopedTempDirectory temp;
            const DataPaths first { temp.child ("home") };
            auto owner = first.acquireAppLock();
            expect (owner != nullptr);
            expect (first.acquireAppLock() == nullptr, "a second handle must not acquire the same home");
            const DataPaths alias { juce::File (first.home.getFullPathName().toUpperCase()) };
            expect (alias.acquireAppLock() == nullptr, "different casing must not bypass ownership");
            const DataPaths separate { temp.child ("other-home") };
            auto independent = separate.acquireAppLock();
            expect (independent != nullptr, "separate homes may run concurrently");
            expect (! first.home.getChildFile (".snds.lock").deleteFile(),
                    "the lock must not be replaceable while held");
            owner.reset();
            owner = first.acquireAppLock();
            expect (owner != nullptr, "a leftover lock file must not prevent reopening");
        }

        beginTest ("An unusable home cannot acquire app ownership");
        {
            const test::ScopedTempDirectory temp;
            const auto file = temp.child ("file");
            expect (file.replaceWithText ("not a directory"));
            expect (DataPaths { file }.acquireAppLock() == nullptr);
        }
    }

private:
    static juce::String home (const juce::String& sndsHome, const juce::String& localAppData, const juce::File& cwd)
    {
        const auto paths = DataPaths::resolve (sndsHome, localAppData, cwd);
        return paths.has_value() ? paths->home.getFullPathName() : juce::String ("<none>");
    }
};

DataPathsTests dataPathsTests;
} // namespace
} // namespace snds
