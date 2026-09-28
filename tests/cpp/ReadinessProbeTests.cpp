#include "TestUtilities.h"
#include "app/ReadinessProbe.h"

namespace snds::test
{

class ReadinessProbeTests final : public juce::UnitTest
{
public:
    ReadinessProbeTests()
        : juce::UnitTest ("Readiness probe", testCategory)
    {
    }

    void runTest() override
    {
        beginTest ("Worker project is found above the build output");
        {
            const ScopedTempDirectory temp;
            temp.createFile ("checkout/worker/pyproject.toml");
            const auto exe = temp.createFile ("checkout/build/vs2022/bin/Debug/SNDS.exe");

            LocalEnvironment env;
            env.executableFile = exe;

            const auto location = LocalReadinessProbe::locateWorkerDirectory (env);
            expect (location.directory == temp.get().getChildFile ("checkout/worker"), location.source);
        }

        beginTest ("SNDS_WORKER_DIR override is used and validated");
        {
            const ScopedTempDirectory temp;
            temp.createFile ("elsewhere/worker/pyproject.toml");
            const auto exe = temp.createFile ("installed/SNDS.exe");

            LocalEnvironment env;
            env.executableFile = exe;
            env.workerDirOverride = temp.get().getChildFile ("elsewhere/worker").getFullPathName();
            expect (LocalReadinessProbe::locateWorkerDirectory (env).directory
                    == temp.get().getChildFile ("elsewhere/worker"));

            env.workerDirOverride = temp.get().getChildFile ("installed").getFullPathName();
            const auto invalid = LocalReadinessProbe::locateWorkerDirectory (env);
            expect (invalid.directory == juce::File());
            expect (invalid.source.contains ("SNDS_WORKER_DIR"), invalid.source);

            env.workerDirOverride = "relative\\worker";
            expect (LocalReadinessProbe::locateWorkerDirectory (env).source.contains ("absolute"));

            env.workerDirOverride = {};
            expect (LocalReadinessProbe::locateWorkerDirectory (env).directory == juce::File(),
                    "No checkout above the executable");
        }

        beginTest ("PATH search handles quotes, blanks and relative entries");
        {
            const ScopedTempDirectory temp;
            const auto uv = temp.createFile ("tools dir/uv.exe");
            const auto path = "relative\\bin;;\"" + temp.get().getChildFile ("empty").getFullPathName() + "\";\""
                              + uv.getParentDirectory().getFullPathName() + "\"";

            expect (LocalReadinessProbe::findOnPath (path, "uv.exe") == uv);
            expect (LocalReadinessProbe::findOnPath (path, "missing.exe") == juce::File());
            expect (LocalReadinessProbe::findOnPath ({}, "uv.exe") == juce::File());
        }

        beginTest ("Missing setup produces actionable checks");
        {
            const ScopedTempDirectory temp;
            temp.createFile ("checkout/worker/pyproject.toml");

            LocalEnvironment env;
            env.executableFile = temp.createFile ("checkout/build/SNDS.exe");
            env.modelsDirectory = temp.get().getChildFile ("data/models");

            const auto report = LocalReadinessProbe (env).run();
            expectEquals (static_cast<int> (report.checks.size()), 4);
            expect (! report.isSeparationAvailable());

            expect (statusOf (report, "worker-project") == ReadinessStatus::ok);
            expect (statusOf (report, "uv") == ReadinessStatus::missing);
            expect (statusOf (report, "worker-environment") == ReadinessStatus::missing);
            expect (statusOf (report, "model-directory") == ReadinessStatus::missing);

            for (const auto& check : report.checks)
                if (check.status != ReadinessStatus::ok)
                    expect (check.action.isNotEmpty(), check.id + " needs guidance");

            expect (report.find ("worker-environment")->action.contains ("scripts\\bootstrap.ps1"));
        }

        beginTest ("Complete setup reports ready");
        {
            const ScopedTempDirectory temp;
            temp.createFile ("checkout/worker/pyproject.toml");
            temp.createFile ("checkout/worker/.venv/Scripts/python.exe");
            const auto uv = temp.createFile ("bin/uv.exe");
            temp.get().getChildFile ("data/models").createDirectory();

            LocalEnvironment env;
            env.executableFile = temp.createFile ("checkout/build/SNDS.exe");
            env.pathVariable = uv.getParentDirectory().getFullPathName();
            env.modelsDirectory = temp.get().getChildFile ("data/models");

            const auto report = LocalReadinessProbe (env).run();
            expect (report.isSeparationAvailable());

            for (const auto& check : report.checks)
                expect (check.status == ReadinessStatus::ok, check.id + ": " + check.detail);
        }

        beginTest ("Worker environment is unknown without a worker project");
        {
            const ScopedTempDirectory temp;
            LocalEnvironment env;
            env.executableFile = temp.createFile ("lonely/SNDS.exe");
            env.modelsDirectory = temp.get().getChildFile ("models");

            const auto report = LocalReadinessProbe (env).run();
            expect (statusOf (report, "worker-project") == ReadinessStatus::missing);
            expect (statusOf (report, "worker-environment") == ReadinessStatus::missing);
            expect (ReadinessReport().isSeparationAvailable() == false, "An empty report is never ready");
        }
    }

private:
    ReadinessStatus statusOf (const ReadinessReport& report, const char* id)
    {
        const auto* check = report.find (id);
        expect (check != nullptr, juce::String ("missing check ") + id);
        return check != nullptr ? check->status : ReadinessStatus::warning;
    }
};

static ReadinessProbeTests readinessProbeTests;

} // namespace snds::test
