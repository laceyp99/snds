#include <juce_core/juce_core.h>

#include "DspHeavy.h"
#include "TestHelpers.h"
#include "app/WorkerProcess.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <deque>
#include <mutex>

namespace snds
{
namespace
{
using namespace worker;

/** Collects what a WorkerProcess delivers. The dispatcher queues closures; pump() runs them on
    the calling (test) thread, standing in for the message thread. */
class Harness
{
public:
    WorkerProcess::Dispatcher dispatcher()
    {
        return [this] (std::function<void()> work)
        {
            const std::lock_guard<std::mutex> lock (mutex);
            queue.push_back (std::move (work));
        };
    }

    WorkerProcess::Callbacks callbacks()
    {
        return { [this] (const Event& event) { events.push_back (event); },
                 [this] (const WorkerJobResult& done) { result = done; } };
    }

    /** Runs queued deliveries until the predicate holds or the time runs out. */
    template <typename Predicate>
    bool pumpUntil (Predicate&& done, int timeoutMs)
    {
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;

        for (;;)
        {
            drain();

            if (done())
                return true;

            if (juce::Time::getMillisecondCounterHiRes() > deadline)
                return false;

            juce::Thread::sleep (5);
        }
    }

    bool pumpUntilFinished (int timeoutMs)
    {
        return pumpUntil ([this] { return result.has_value(); }, timeoutMs);
    }

    template <typename T>
    int count() const
    {
        int total = 0;

        for (const auto& event : events)
            total += std::holds_alternative<T> (event) ? 1 : 0;

        return total;
    }

    std::vector<Event> events;
    std::optional<WorkerJobResult> result;

private:
    void drain()
    {
        for (;;)
        {
            std::function<void()> work;

            {
                const std::lock_guard<std::mutex> lock (mutex);

                if (queue.empty())
                    return;

                work = std::move (queue.front());
                queue.pop_front();
            }

            work();
        }
    }

    std::mutex mutex;
    std::deque<std::function<void()>> queue;
};

bool processIsAlive (DWORD pid)
{
    const HANDLE handle = OpenProcess (SYNCHRONIZE, FALSE, pid);

    if (handle == nullptr)
        return false;

    const auto alive = WaitForSingleObject (handle, 0) == WAIT_TIMEOUT;
    CloseHandle (handle);
    return alive;
}

class WorkerProcessTests final : public juce::UnitTest
{
public:
    WorkerProcessTests()
        : juce::UnitTest ("Worker process", "snds")
    {
    }

    void runTest() override
    {
        beginTest ("Setup");
        runHelperTests();

        const test::ScopedTempDirectory temp;
        fakePython = temp.child ("worker\\.venv\\Scripts\\python.exe");
        fakePython.getParentDirectory().createDirectory();
        expect (juce::File::getSpecialLocation (juce::File::currentExecutableFile).copyFileTo (fakePython));
        jobsRoot = temp.child ("jobs");

        runLifecycleTests();
        runFailureTests();
        runCancellationTests();
        runTreeTests();
    }

private:
    juce::File fakePython;
    juce::File jobsRoot;
    int jobCounter = 0;

    struct Job
    {
        juce::File dir;
        juce::StringPairArray environment;
        int cancelGraceMs = 500;
        int timeoutMs = 0;
    };

    Job newJob (const juce::String& mode)
    {
        Job job;
        job.dir = jobsRoot.getChildFile ("job" + juce::String (++jobCounter));
        job.dir.createDirectory();
        job.environment.set ("SNDS_FAKE_WORKER", mode);
        job.environment.set ("SNDS_FAKE_MARKER", "from-options");
        return job;
    }

    WorkerProcessOptions optionsFor (const Job& job) const
    {
        WorkerProcessOptions options;
        options.commandLine = { fakePython.getFullPathName(), "-I", "-m", "snds_worker", "prepare" };
        options.workingDirectory = fakePython.getParentDirectory();
        options.requestLine = R"({"protocol":1,"type":"prepare","jobId":"job1"})";
        options.jobDir = job.dir;
        options.extraEnvironment = job.environment;
        options.cancelGraceMs = job.cancelGraceMs;
        options.timeoutMs = job.timeoutMs;
        return options;
    }

    void plantFiles (const Job& job)
    {
        job.dir.getChildFile ("package").createDirectory();
        job.dir.getChildFile ("package\\drums.wav").replaceWithText ("partial audio");
        job.dir.getChildFile ("package\\bass.WAV").replaceWithText ("partial audio");
        job.dir.getChildFile ("notes.log").replaceWithText ("keep me");
    }

    //==========================================================================
    void runHelperTests()
    {
        beginTest ("Command-line arguments are quoted for CommandLineToArgvW");
        expectEquals (quoteCommandLineArgument ("plain"), juce::String ("plain"));
        expectEquals (quoteCommandLineArgument ("with space"), juce::String ("\"with space\""));
        expectEquals (quoteCommandLineArgument ({}), juce::String ("\"\""));
        expectEquals (quoteCommandLineArgument ("say \"hi\""), juce::String ("\"say \\\"hi\\\"\""));
        expectEquals (quoteCommandLineArgument ("C:\\dir with space\\"), juce::String ("\"C:\\dir with space\\\\\""));

        beginTest ("Incomplete audio is deleted and logs are kept");
        const test::ScopedTempDirectory dir;
        dir.child ("a\\b").createDirectory();
        dir.child ("a\\b\\stem.wav").replaceWithText ("x");
        dir.child ("a\\mix.mp3").replaceWithText ("x");
        dir.child ("worker-stderr.log").replaceWithText ("log");
        dir.child ("artwork.png").replaceWithText ("png");
        expectEquals (deleteIncompleteAudio (dir.get()), 2);
        expect (dir.child ("worker-stderr.log").existsAsFile());
        expect (dir.child ("artwork.png").existsAsFile());
        expect (! dir.child ("a").exists(), "empty folders are removed");
        expectEquals (deleteIncompleteAudio (dir.child ("missing")), 0);
    }

    //==========================================================================
    void runLifecycleTests()
    {
        beginTest ("A normal run delivers stage, progress, and result events in order");
        {
            auto job = newJob ("ok");
            plantFiles (job);
            Harness harness;
            WorkerProcess process (optionsFor (job), harness.callbacks(), harness.dispatcher());
            process.start();
            expect (harness.pumpUntilFinished (30000));
            expect (harness.result.has_value());

            if (harness.result)
            {
                const auto& result = *harness.result;
                expect (result.launched);
                expectEquals (result.exitCode, 0);
                expect (result.outcome == JobOutcome::succeeded, describe (result.outcome));
                expect (result.terminalKind == TerminalKind::result);
                expect (! result.killedByApp);
                expectEquals (result.deletedAudioFiles, 0);
                expect (result.stderrTail.contains ("fake worker stderr line"));
                expect (result.stderrTail.contains ("from-options"), "extra environment reaches the child");
            }

            expect (harness.events.size() == 6);
            expect (std::holds_alternative<AcceptedEvent> (harness.events.front()));
            expect (std::holds_alternative<ResultEvent> (harness.events.back()));
            expectEquals (harness.count<StageEvent>(), 2);
            expectEquals (harness.count<ProgressEvent>(), 2);
            expect (job.dir.getChildFile ("package\\drums.wav").existsAsFile(), "nothing is cleaned after success");
            expect (job.dir.getChildFile ("worker-stderr.log").existsAsFile());
            expect (job.dir.getChildFile ("worker-stderr.log").loadFileAsString().contains ("fake worker stderr"));
            expect (! process.isRunning());
        }

        beginTest ("Malformed lines and unknown events are counted and skipped");
        {
            auto job = newJob ("messy");
            Harness harness;
            WorkerProcess process (optionsFor (job), harness.callbacks(), harness.dispatcher());
            process.start();
            expect (harness.pumpUntilFinished (30000));

            if (harness.result)
            {
                expect (harness.result->outcome == JobOutcome::succeeded);
                expectEquals (harness.result->invalidLines, 2);
                expectEquals (harness.result->unknownEvents, 1);
            }

            expectEquals (harness.count<ResultEvent>(), 1);
            expectEquals (harness.count<InvalidLine>(), 0);
            expectEquals (harness.count<UnknownEvent>(), 0);
        }

        beginTest ("Output far beyond the pipe buffers does not stall the worker");
        {
            auto job = newJob ("flood");
            Harness harness;
            WorkerProcess process (optionsFor (job), harness.callbacks(), harness.dispatcher());
            process.start();
            expect (harness.pumpUntilFinished (60000));
            // 4000 progress lines (about 280 KB, several times the 64 KB pipe buffer) by default; 20000 with
            // SNDS_HEAVY_TESTS=1.
            expectEquals (harness.count<ProgressEvent>(), test::workload (4000, 20000));

            if (harness.result)
                expect (harness.result->outcome == JobOutcome::succeeded);
        }
    }

    //==========================================================================
    void runFailureTests()
    {
        beginTest ("A reported failure ends as failed, with details, and removes partial audio");
        {
            auto job = newJob ("fail");
            plantFiles (job);
            Harness harness;
            WorkerProcess process (optionsFor (job), harness.callbacks(), harness.dispatcher());
            process.start();
            expect (harness.pumpUntilFinished (30000));

            if (harness.result)
            {
                expectEquals (harness.result->exitCode, 13);
                expect (harness.result->outcome == JobOutcome::failed);
                expectEquals (harness.result->deletedAudioFiles, 2);

                if (harness.result->terminalEvent)
                {
                    const auto* failure = std::get_if<FailureEvent> (&*harness.result->terminalEvent);
                    expect (failure != nullptr);

                    if (failure != nullptr)
                        expectEquals (failure->code, juce::String ("decode_failed"));
                }
            }

            expect (! job.dir.getChildFile ("package\\drums.wav").existsAsFile());
            expect (! job.dir.getChildFile ("package\\bass.WAV").existsAsFile());
            expect (job.dir.getChildFile ("notes.log").existsAsFile(), "logs are preserved");
            expect (job.dir.getChildFile ("worker-stderr.log").existsAsFile());
        }

        beginTest ("An unsupported protocol stops the job and is reported");
        {
            auto job = newJob ("protocol2");
            Harness harness;
            WorkerProcess process (optionsFor (job), harness.callbacks(), harness.dispatcher());
            process.start();
            expect (harness.pumpUntilFinished (30000));

            if (harness.result)
            {
                expect (harness.result->outcome == JobOutcome::protocolViolation);
                expect (harness.result->protocolError.contains ("protocol 2"), harness.result->protocolError);
                expect (harness.result->killedByApp);
                expect (harness.result->seconds < 10.0);
            }
        }

        beginTest ("A crash is a crash, whatever was on stdout");
        {
            auto job = newJob ("crash");
            Harness harness;
            WorkerProcess process (optionsFor (job), harness.callbacks(), harness.dispatcher());
            process.start();
            expect (harness.pumpUntilFinished (30000));

            if (harness.result)
            {
                expect (harness.result->outcome == JobOutcome::crashed);
                expectEquals (static_cast<juce::uint32> (harness.result->exitCode),
                              static_cast<juce::uint32> (0xC0000005));
            }
        }

        beginTest ("Exit 0 without a result is a protocol violation");
        {
            auto job = newJob ("silent-exit");
            Harness harness;
            WorkerProcess process (optionsFor (job), harness.callbacks(), harness.dispatcher());
            process.start();
            expect (harness.pumpUntilFinished (30000));

            if (harness.result)
                expect (harness.result->outcome == JobOutcome::protocolViolation);
        }

        beginTest ("A program that cannot be started is reported, not thrown");
        {
            auto job = newJob ("ok");
            auto options = optionsFor (job);
            options.commandLine.set (0, jobsRoot.getChildFile ("nonexistent\\python.exe").getFullPathName());
            Harness harness;
            WorkerProcess process (options, harness.callbacks(), harness.dispatcher());
            process.start();
            expect (harness.pumpUntilFinished (30000));

            if (harness.result)
            {
                expect (! harness.result->launched);
                expect (harness.result->launchError.isNotEmpty());
            }
        }
    }

    //==========================================================================
    void runCancellationTests()
    {
        beginTest ("A cooperative worker acknowledges the cancel line and exits 12");
        {
            auto job = newJob ("cooperative");
            Harness harness;
            WorkerProcess process (optionsFor (job), harness.callbacks(), harness.dispatcher());
            process.start();
            expect (harness.pumpUntil ([&] { return harness.count<ProgressEvent>() > 2; }, 30000));
            process.cancel();
            expect (harness.pumpUntilFinished (30000));

            if (harness.result)
            {
                expect (harness.result->cancelRequested);
                expect (harness.result->outcome == JobOutcome::cancelled, describe (harness.result->outcome));
                expectEquals (harness.result->exitCode, 12);
                expect (! harness.result->killedByApp);
                expectEquals (harness.count<CancelledEvent>(), 1);
            }
        }

        beginTest ("A worker that ignores cancel is terminated after the grace period");
        {
            auto job = newJob ("hang");
            job.cancelGraceMs = 400;
            Harness harness;
            WorkerProcess process (optionsFor (job), harness.callbacks(), harness.dispatcher());
            process.start();
            expect (harness.pumpUntil ([&] { return harness.count<StageEvent>() > 0; }, 30000));
            const auto cancelledAt = juce::Time::getMillisecondCounterHiRes();
            process.cancel();
            expect (harness.pumpUntilFinished (30000));
            const auto waited = juce::Time::getMillisecondCounterHiRes() - cancelledAt;

            if (harness.result)
            {
                expect (harness.result->outcome == JobOutcome::killed, describe (harness.result->outcome));
                expectEquals (harness.result->exitCode, exitcode::killedByApp);
                expect (harness.result->killedByApp);
            }

            expect (waited >= 400.0 && waited < 15000.0, juce::String (waited));
        }

        beginTest ("A hung worker is cancelled and then killed at the overall timeout");
        {
            auto job = newJob ("hang");
            job.cancelGraceMs = 300;
            job.timeoutMs = 600;
            plantFiles (job);
            Harness harness;
            WorkerProcess process (optionsFor (job), harness.callbacks(), harness.dispatcher());
            process.start();
            expect (harness.pumpUntilFinished (30000));

            if (harness.result)
            {
                expect (harness.result->timedOut);
                expect (harness.result->outcome == JobOutcome::killed);
                expect (harness.result->seconds >= 0.9 && harness.result->seconds < 15.0,
                        juce::String (harness.result->seconds));
                expectEquals (harness.result->deletedAudioFiles, 2);
            }
        }
    }

    //==========================================================================
    void runTreeTests()
    {
        beginTest ("Killing the job also kills a grandchild process");
        {
            auto job = newJob ("grandchild");
            job.cancelGraceMs = 300;
            const auto pidFile = job.dir.getChildFile ("grandchild.pid");
            job.environment.set ("SNDS_FAKE_PIDFILE", pidFile.getFullPathName());
            Harness harness;
            WorkerProcess process (optionsFor (job), harness.callbacks(), harness.dispatcher());
            process.start();
            expect (harness.pumpUntil ([&] { return harness.count<StageEvent>() > 0; }, 30000));

            const auto pid = static_cast<DWORD> (pidFile.loadFileAsString().trim().getIntValue());
            expect (pid != 0, "grandchild pid was recorded");
            expect (processIsAlive (pid), "grandchild runs while the job runs");

            process.cancel();
            expect (harness.pumpUntilFinished (30000), "the job ends although the grandchild holds the pipes");
            expect (harness.result.has_value() && harness.result->outcome == JobOutcome::killed);
            juce::Thread::sleep (200);
            expect (! processIsAlive (pid), "grandchild was terminated with the tree");
        }

        beginTest ("Destroying the owner terminates the tree and delivers nothing afterwards");
        {
            auto job = newJob ("grandchild");
            const auto pidFile = job.dir.getChildFile ("grandchild.pid");
            job.environment.set ("SNDS_FAKE_PIDFILE", pidFile.getFullPathName());
            Harness harness;
            DWORD pid = 0;
            const auto before = juce::Time::getMillisecondCounterHiRes();

            {
                WorkerProcess process (optionsFor (job), harness.callbacks(), harness.dispatcher());
                process.start();
                expect (harness.pumpUntil ([&] { return harness.count<StageEvent>() > 0; }, 30000));
                pid = static_cast<DWORD> (pidFile.loadFileAsString().trim().getIntValue());
                expect (processIsAlive (pid));
            }

            expect (juce::Time::getMillisecondCounterHiRes() - before < 20000.0);
            juce::Thread::sleep (200);
            expect (! processIsAlive (pid), "grandchild is gone");
            harness.pumpUntil ([] { return false; }, 100);
            expect (! harness.result.has_value(), "no callback after destruction");
        }
    }
};

WorkerProcessTests workerProcessTests;
} // namespace
} // namespace snds
