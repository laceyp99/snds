#include <juce_core/juce_core.h>

#include "TestHelpers.h"
#include "app/Readiness.h"
#include "app/WorkerLocation.h"

#include <cstdlib>

namespace snds
{
namespace
{
using State = Readiness::State;

const WorkerLocation testLocation { juce::File ("C:\\snds\\worker"), "build default" };

// The shape of a real report from a ready CUDA environment.
juce::var readyReport()
{
    return juce::JSON::parse (R"({
        "protocol": 1, "type": "readiness", "ready": true,
        "worker": {"version": "0.0.1", "python": "3.11.15", "executable": "C:\\snds\\worker\\.venv\\Scripts\\python.exe"},
        "packages": {"numpy": "1.26.4", "soundfile": "0.12.1", "torch": "2.1.2+cu121", "torchaudio": "2.1.2+cu121", "demucs": "4.0.1"},
        "compute": {"torch_build": "cuda", "torch_cuda_version": "12.1", "cuda_available": true,
                    "cuda_device": "NVIDIA GeForce RTX 4050 Laptop GPU", "device": "cuda"},
        "model": {"name": "htdemucs", "signature": "955717e8", "path": "C:\\home\\models\\htdemucs\\955717e8-8726e21a.th",
                  "present": true, "size_bytes": 84141911, "expected_size_bytes": 84141911, "sha256_checked": false},
        "paths": {"home": "C:\\home"},
        "problems": [],
        "elapsed_ms": 2125})");
}

juce::var problem (const juce::String& code, const juce::String& message, const juce::String& details = {})
{
    auto* object = new juce::DynamicObject();
    object->setProperty ("code", code);
    object->setProperty ("message", message);

    if (details.isNotEmpty())
        object->setProperty ("details", details);

    return juce::var (object);
}

juce::var withProblems (juce::var report, const juce::Array<juce::var>& problems)
{
    report.getDynamicObject()->setProperty ("problems", problems);
    report.getDynamicObject()->setProperty ("ready", problems.isEmpty());
    return report;
}

juce::var withProperty (juce::var report, const juce::String& section, const juce::String& name, const juce::var& value)
{
    if (section.isEmpty())
        report.getDynamicObject()->setProperty (name, value);
    else
        report[juce::Identifier (section)].getDynamicObject()->setProperty (name, value);

    return report;
}

Readiness interpretOutput (const juce::String& output, int exitCode)
{
    ReadinessRun run;
    run.outcome = ReadinessRun::Outcome::finished;
    run.exitCode = exitCode;
    run.output = output;
    run.seconds = 2.5;
    return interpretReadiness (run, testLocation);
}

Readiness interpretReport (const juce::var& report, int exitCode)
{
    return interpretOutput (juce::JSON::toString (report, true) + "\n", exitCode);
}

Readiness interpretOutcome (ReadinessRun::Outcome outcome, double seconds = 1.0)
{
    ReadinessRun run;
    run.outcome = outcome;
    run.seconds = seconds;
    return interpretReadiness (run, testLocation);
}

//==============================================================================
class WorkerLocationTests final : public juce::UnitTest
{
public:
    WorkerLocationTests()
        : juce::UnitTest ("WorkerLocation", "snds")
    {
    }

    void runTest() override
    {
        const juce::File cwd ("C:\\cwd");
        const juce::File buildDefault ("C:\\src\\snds\\worker");

        beginTest ("--worker-root wins over SNDS_WORKER_ROOT and the build default");
        {
            const auto location =
                WorkerLocation::resolve (juce::File ("D:\\cli\\worker"), "E:\\env\\worker", cwd, buildDefault);
            expectEquals (location.root.getFullPathName(), juce::String ("D:\\cli\\worker"));
            expectEquals (location.source, juce::String ("--worker-root"));
        }

        beginTest ("SNDS_WORKER_ROOT wins over the build default; relative values use the working directory");
        {
            const auto absolute = WorkerLocation::resolve ({}, "\"E:\\env\\worker\"", cwd, buildDefault);
            expectEquals (absolute.root.getFullPathName(), juce::String ("E:\\env\\worker"));
            expectEquals (absolute.source, juce::String (WorkerLocation::rootVariable));

            const auto relative = WorkerLocation::resolve ({}, "..\\other\\worker", cwd, buildDefault);
            expectEquals (relative.root.getFullPathName(), juce::String ("C:\\other\\worker"));
        }

        beginTest ("The build default is used last");
        {
            const auto location = WorkerLocation::resolve ({}, "  ", cwd, buildDefault);
            expect (location.root == buildDefault);
            expectEquals (location.source, juce::String ("build default"));
        }

        beginTest ("The worker runs isolated from the venv interpreter");
        expectEquals (testLocation.pythonExecutable().getFullPathName(),
                      juce::String ("C:\\snds\\worker\\.venv\\Scripts\\python.exe"));
        expectEquals (testLocation.commandFor ("readiness").joinIntoString (" "),
                      juce::String ("C:\\snds\\worker\\.venv\\Scripts\\python.exe -I -m snds_worker readiness"));

        beginTest ("The build default points at this checkout's worker folder");
        {
            const auto location = WorkerLocation::fromEnvironment ({});

            if (juce::SystemStats::getEnvironmentVariable (WorkerLocation::rootVariable, {}).isEmpty())
                expect (location.root.getChildFile ("pyproject.toml").existsAsFile(), location.root.getFullPathName());
        }
    }
};

//==============================================================================
class ReadinessInterpretationTests final : public juce::UnitTest
{
public:
    ReadinessInterpretationTests()
        : juce::UnitTest ("Readiness interpretation", "snds")
    {
    }

    void runTest() override
    {
        beginTest ("A ready CUDA environment names the GPU");
        {
            const auto readiness = interpretReport (readyReport(), 0);
            expect (readiness.state == State::ready);
            expect (readiness.isReady());
            expectEquals (readiness.headline,
                          juce::String ("Worker ready: CUDA on NVIDIA GeForce RTX 4050 Laptop GPU"));
            expect (readiness.guidance.isEmpty());
            expect (readiness.details.contains ("torch 2.1.2+cu121"));
            expect (readiness.details.contains ("Python 3.11.15"));
        }

        beginTest ("A ready CPU environment says CPU");
        {
            auto report = withProperty (readyReport(), "compute", "torch_build", "cpu");
            report = withProperty (report, "compute", "device", "cpu");
            expectEquals (interpretReport (report, 0).headline, juce::String ("Worker ready: CPU"));
        }

        beginTest ("A CUDA build without a usable GPU falls back to the CPU");
        {
            auto report = withProperty (readyReport(), "compute", "device", "cpu");
            report = withProperty (report, "compute", "cuda_available", false);
            const auto readiness = interpretReport (report, 0);
            expect (readiness.isReady());
            expectEquals (readiness.headline, juce::String ("Worker ready: CPU (no usable NVIDIA GPU)"));
        }

        beginTest ("A missing model is 'model missing', not an error, despite exit code 1");
        {
            const auto readiness = interpretReport (
                withProblems (readyReport(),
                              { problem ("model_missing", "The htdemucs checkpoint is not installed.") }),
                1);
            expect (readiness.state == State::modelMissing);
            expectEquals (readiness.headline, juce::String ("The separation model is not installed."));
            expect (readiness.guidance.contains ("scripts\\bootstrap.ps1"));
            expect (readiness.details.contains ("[model_missing]"));
        }

        beginTest ("A truncated model is 'model missing' with its own headline");
        {
            const auto readiness = interpretReport (
                withProblems (readyReport(), { problem ("model_size_mismatch", "The checkpoint has 1234 bytes.") }), 1);
            expect (readiness.state == State::modelMissing);
            expect (readiness.headline.contains ("damaged or incomplete"));
        }

        beginTest ("Every beat-detection and hash problem code maps to 'model missing'");
        {
            for (const auto* code : { "model_hash_mismatch",
                                      "bpm_model_missing",
                                      "bpm_model_size_mismatch",
                                      "bpm_model_hash_mismatch",
                                      "bpm_model_unreadable" })
            {
                const auto readiness = interpretReport (withProblems (readyReport(), { problem (code, "x") }), 1);
                expect (readiness.state == State::modelMissing, code);
                expect (readiness.guidance.contains ("scripts\\bootstrap.ps1"), code);
            }

            expectEquals (
                interpretReport (withProblems (readyReport(), { problem ("bpm_model_missing", "x") }), 1).headline,
                juce::String ("The beat-detection model is not installed."));
            expect (interpretReport (withProblems (readyReport(), { problem ("bpm_model_hash_mismatch", "x") }), 1)
                        .headline.contains ("beat-detection model file is damaged"));
            expect (interpretReport (withProblems (readyReport(), { problem ("model_hash_mismatch", "x") }), 1)
                        .headline.contains ("separation model file is damaged"));
        }

        beginTest ("No usable CPU is an error, and the new additive fields are tolerated");
        {
            const auto readiness = interpretReport (
                withProblems (readyReport(), { problem ("cpu_unavailable", "The CPU path is unusable.") }), 1);
            expect (readiness.state == State::error);
            expectEquals (readiness.headline, juce::String ("The CPU path is unusable."));

            auto report = withProperty (readyReport(), {}, "bpm_model", juce::JSON::parse (R"({"present":true})"));
            report = withProperty (report, "compute", "cpu_available", true);
            report = withProperty (report, "compute", "cuda_healthy", true);
            report = withProperty (report, "compute", "cuda_check_ms", 120);
            report = withProperty (report, "model", "verified", true);
            expect (interpretReport (report, 0).isReady());
        }

        beginTest ("CUDA that is installed but unhealthy reports CPU");
        {
            auto report = withProperty (readyReport(), "compute", "device", "cpu");
            report = withProperty (report, "compute", "cuda_healthy", false);
            expectEquals (interpretReport (report, 0).headline,
                          juce::String ("Worker ready: CPU (no usable NVIDIA GPU)"));
        }

        beginTest ("Missing packages mean the environment is not set up");
        {
            const auto readiness =
                interpretReport (withProblems (readyReport(),
                                               { problem ("package_missing", "torch is not installed."),
                                                 problem ("model_missing", "Not installed.") }),
                                 1);
            expect (readiness.state == State::notSetUp);
            expect (readiness.guidance.contains ("scripts\\bootstrap.ps1"));
        }

        beginTest ("Import failures are errors with the traceback in the details");
        {
            const auto readiness = interpretReport (
                withProblems (
                    readyReport(),
                    { problem ("import_failed",
                               "torch is installed but failed to import (OSError: [WinError 126] fbgemm.dll).",
                               "Traceback (most recent call last):\n  File \"x.py\", line 1\nOSError: [WinError 126]"),
                      problem ("model_missing", "Not installed.") }),
                1);
            expect (readiness.state == State::error);
            expect (readiness.headline.startsWith ("torch is installed but failed to import"));
            expect (readiness.guidance.contains ("scripts\\diagnostics.ps1"));
            expect (readiness.details.contains ("    Traceback (most recent call last):"));
        }

        beginTest ("An unresolvable data folder is an error");
        {
            const auto readiness = interpretReport (
                withProblems (readyReport(),
                              { problem ("home_unavailable", "Neither SNDS_HOME nor LOCALAPPDATA is set.") }),
                1);
            expect (readiness.state == State::error);
            expectEquals (readiness.headline, juce::String ("Neither SNDS_HOME nor LOCALAPPDATA is set."));
        }

        beginTest ("An internal worker error is an error");
        {
            const auto readiness = interpretOutput (
                R"({"protocol": 1, "type": "error", "command": "readiness", "problems": [{"code": "internal_error", "message": "RuntimeError: boom"}]})",
                3);
            expect (readiness.state == State::error);
            expectEquals (readiness.headline,
                          juce::String ("The readiness check failed inside the worker: RuntimeError: boom"));
        }

        beginTest ("An unknown protocol version is an error");
        expect (interpretReport (withProperty (readyReport(), {}, "protocol", 2), 0).state == State::error);

        beginTest ("'Not ready' without problems is an error");
        expect (interpretReport (withProperty (readyReport(), {}, "ready", false), 1).state == State::error);

        beginTest ("Stray lines around the report are tolerated");
        expect (interpretOutput ("warming up\n" + juce::JSON::toString (readyReport(), true) + "\n\n", 0).isReady());

        beginTest ("Output without a report is an error that includes the output");
        {
            const auto readiness = interpretOutput ("Traceback: something broke", 1);
            expect (readiness.state == State::error);
            expect (readiness.headline.contains ("without a readiness report"));
            expect (readiness.details.contains ("Traceback: something broke"));
        }

        beginTest ("Launch outcomes map to states with guidance");
        {
            const auto missing = interpretOutcome (ReadinessRun::Outcome::environmentMissing);
            expect (missing.state == State::notSetUp);
            expect (missing.guidance.contains ("scripts\\bootstrap.ps1"));
            expect (missing.details.contains ("C:\\snds\\worker\\.venv\\Scripts\\python.exe"));

            expect (interpretOutcome (ReadinessRun::Outcome::failedToStart).state == State::error);

            const auto timedOut = interpretOutcome (ReadinessRun::Outcome::timedOut, 90.2);
            expect (timedOut.state == State::error);
            expect (timedOut.headline.contains ("90 seconds"));
            expect (timedOut.guidance.contains ("scripts\\diagnostics.ps1"));
        }

        beginTest ("State names are stable for logs and automation");
        expectEquals (Readiness::stateName (State::notSetUp), juce::String ("not-set-up"));
        expectEquals (Readiness::stateName (State::modelMissing), juce::String ("model-missing"));
    }
};

//==============================================================================
// Runs the launcher against real processes: this test executable, copied into a fake worker
// environment as python.exe, plays the worker (see runFakeWorker in TestMain.cpp).
class ReadinessProcessTests final : public juce::UnitTest
{
public:
    ReadinessProcessTests()
        : juce::UnitTest ("Readiness process", "snds")
    {
    }

    void runTest() override
    {
        beginTest ("Setup");
        const test::ScopedTempDirectory temp;
        const WorkerLocation fake { temp.child ("worker"), "test" };
        fake.pythonExecutable().getParentDirectory().createDirectory();
        expect (
            juce::File::getSpecialLocation (juce::File::currentExecutableFile).copyFileTo (fake.pythonExecutable()));

        beginTest ("A report on stdout is read and interpreted");
        {
            const auto run = runWith ("ready", fake, 30000);
            expect (run.outcome == ReadinessRun::Outcome::finished);
            expectEquals (run.exitCode, 0);
            expectEquals (interpretReadiness (run, fake).headline, juce::String ("Worker ready: CUDA on Fake GPU"));
        }

        beginTest ("Output larger than the pipe buffer does not deadlock");
        {
            const auto run = runWith ("large", fake, 30000);
            expect (run.outcome == ReadinessRun::Outcome::finished);
            expect (run.output.length() > 300000);
            expect (interpretReadiness (run, fake).isReady());
        }

        beginTest ("A worker that prints no report is an error");
        {
            const auto run = runWith ("garbage", fake, 30000);
            expectEquals (run.exitCode, 3);
            expect (interpretReadiness (run, fake).state == State::error);
        }

        beginTest ("A hung worker is killed at the timeout");
        {
            const auto run = runWith ("hang", fake, 1000);
            expect (run.outcome == ReadinessRun::Outcome::timedOut);
            expect (run.seconds >= 1.0 && run.seconds < 10.0, juce::String (run.seconds));
        }

        beginTest ("A check can be cancelled");
        {
            const auto started = juce::Time::getMillisecondCounterHiRes();
            const auto run = runWith (
                "hang", fake, 60000, [started] { return juce::Time::getMillisecondCounterHiRes() - started > 300; });
            expect (run.outcome == ReadinessRun::Outcome::cancelled);
            expect (run.seconds < 10.0, juce::String (run.seconds));
        }

        beginTest ("A missing environment is reported without starting anything");
        {
            const WorkerLocation missing { temp.child ("nothing-here"), "test" };
            const auto run = runReadinessCommand (missing, 1000);
            expect (run.outcome == ReadinessRun::Outcome::environmentMissing);
        }

        beginTest ("ReadinessChecker reports on its thread, and destroying it kills a hung worker");
        {
            setMode ("ready");
            juce::WaitableEvent finished;
            Readiness received;

            {
                ReadinessChecker checker (fake,
                                          30000,
                                          [&] (const Readiness& readiness)
                                          {
                                              received = readiness;
                                              finished.signal();
                                          });
                expect (finished.wait (30000));
            }

            expect (received.isReady());

            setMode ("hang");
            const auto started = juce::Time::getMillisecondCounterHiRes();
            {
                ReadinessChecker checker (fake, 60000, [] (const Readiness&) {});
                juce::Thread::sleep (200);
            }
            expect (juce::Time::getMillisecondCounterHiRes() - started < 10000);
            setMode ({});
        }
    }

private:
    static void setMode (const juce::String& mode) { _putenv_s ("SNDS_FAKE_WORKER", mode.toRawUTF8()); }

    static ReadinessRun runWith (const juce::String& mode,
                                 const WorkerLocation& location,
                                 int timeoutMs,
                                 std::function<bool()> shouldCancel = {})
    {
        setMode (mode);
        auto run = runReadinessCommand (location, timeoutMs, std::move (shouldCancel));
        setMode ({});
        return run;
    }
};

WorkerLocationTests workerLocationTests;
ReadinessInterpretationTests readinessInterpretationTests;
ReadinessProcessTests readinessProcessTests;
} // namespace
} // namespace snds
