#include "TestUtilities.h"
#include "app/AppPaths.h"

namespace snds::test
{

class AppPathsTests final : public juce::UnitTest
{
public:
    AppPathsTests()
        : juce::UnitTest ("AppPaths", testCategory)
    {
    }

    void runTest() override
    {
        const ScopedTempDirectory temp;
        const auto localAppData = temp.get().getChildFile ("LocalAppData");

        beginTest ("Default root is <LOCALAPPDATA>\\snds");
        {
            expect (AppPaths::resolveRoot (localAppData, {}) == localAppData.getChildFile ("snds"));
            expect (AppPaths::resolveRoot (localAppData, "   ") == localAppData.getChildFile ("snds"));
        }

        beginTest ("Absolute override wins, relative override is ignored");
        {
            const auto custom = temp.get().getChildFile ("custom-root");
            expect (AppPaths::resolveRoot (localAppData, custom.getFullPathName()) == custom);
            expect (AppPaths::resolveRoot (localAppData, "\"" + custom.getFullPathName() + "\"") == custom);
            expect (AppPaths::resolveRoot (localAppData, "relative\\root") == localAppData.getChildFile ("snds"));
        }

        const AppPaths paths (temp.get().getChildFile ("root"));

        beginTest ("Directory getters are pure");
        {
            expect (paths.getDirectory (AppDirectory::settings) == paths.getRoot().getChildFile ("settings"));
            expect (paths.getDirectory (AppDirectory::cache) == paths.getRoot().getChildFile ("cache"));
            expect (paths.getDirectory (AppDirectory::jobs) == paths.getRoot().getChildFile ("jobs"));
            expect (paths.getDirectory (AppDirectory::logs) == paths.getRoot().getChildFile ("logs"));
            expect (paths.getDirectory (AppDirectory::models) == paths.getRoot().getChildFile ("models"));
            expect (! paths.getRoot().exists(), "Resolving paths must not create directories");
        }

        beginTest ("Directories are created on demand");
        {
            expect (paths.ensureDirectory (AppDirectory::logs).wasOk());
            expect (paths.getDirectory (AppDirectory::logs).isDirectory());
            expect (! paths.getDirectory (AppDirectory::cache).exists(), "Only the requested directory is created");
            expect (paths.ensureDirectory (AppDirectory::logs).wasOk(), "Creating twice is harmless");

            expect (paths.ensureAllDirectories().wasOk());

            for (const auto directory : allAppDirectories)
                expect (paths.getDirectory (directory).isDirectory(), AppPaths::getFolderName (directory));
        }

        beginTest ("A file blocking a directory is reported");
        {
            const AppPaths blocked (temp.get().getChildFile ("blocked"));
            temp.createFile ("blocked/jobs", "not a directory");

            const auto result = blocked.ensureDirectory (AppDirectory::jobs);
            expect (result.failed());
            expect (result.getErrorMessage().contains ("file"), result.getErrorMessage());
        }
    }
};

static AppPathsTests appPathsTests;

} // namespace snds::test
