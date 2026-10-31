#include <juce_core/juce_core.h>

#include "SongFixtures.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <process.h>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
/** Prints the log. The runner announces every test ("Starting tests in: ..."/"Completed tests in: ...");
    with SNDS_TEST_TIMES=1 the time between the two is listed at the end, slowest first. */
class ConsoleTestRunner final : public juce::UnitTestRunner
{
public:
    void printTimes() const
    {
        auto sorted = durations;
        std::sort (sorted.begin(), sorted.end(), [] (const auto& a, const auto& b) { return a.second > b.second; });
        double total = 0.0;

        for (const auto& entry : sorted)
            total += entry.second;

        std::cout << "\nTest times (slowest first, 0.5 s and up; total " << juce::String (total, 1) << " s)"
                  << std::endl;

        for (const auto& [name, seconds] : sorted)
            if (seconds >= 0.5)
                std::cout << juce::String (seconds, 1).paddedLeft (' ', 7) << " s  " << name << std::endl;
    }

private:
    void logMessage (const juce::String& message) override
    {
        std::cout << message << std::endl;

        if (message.startsWith ("Starting tests in: "))
        {
            current = message.fromFirstOccurrenceOf ("Starting tests in: ", false, false).trimCharactersAtEnd (".");
            started = juce::Time::getMillisecondCounterHiRes();
        }
        else if (message.startsWith ("Completed tests in ") && current.isNotEmpty())
        {
            durations.emplace_back (current, (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0);
            current = {};
        }
    }

    std::vector<std::pair<juce::String, double>> durations;
    juce::String current;
    double started = 0.0;
};

// ReadinessTests copies this executable into a fake worker environment as python.exe, so the
// readiness launcher can be tested against real processes. SNDS_FAKE_WORKER picks the behaviour.
int runFakeWorker()
{
    const auto mode = juce::SystemStats::getEnvironmentVariable ("SNDS_FAKE_WORKER", {});

    if (mode == "hang")
    {
        juce::Thread::sleep (60000);
        return 0;
    }

    if (mode == "garbage")
    {
        std::cout << "this is not JSON" << std::endl;
        return 3;
    }

    if (mode == "large") // far more output than a pipe buffer holds, before the report
        for (int i = 0; i < 5000; ++i)
            std::cout << "noise " << i << " ............................................................\n";

    std::cout << R"({"protocol": 1, "type": "readiness", "ready": true, "compute": {"torch_build": "cuda", )"
              << R"("device": "cuda", "cuda_device": "Fake GPU"}, "problems": []})" << std::endl;
    return 0;
}

//==============================================================================
// A scripted `prepare` worker. SNDS_FAKE_WORKER picks the script; the app passes it through
// WorkerProcessOptions::extraEnvironment. SNDS_FAKE_PIDFILE (grandchild mode) receives the pid
// of a child process the fake leaves running, to prove the whole tree is killed.
void emit (const std::string& line)
{
    std::cout << line << '\n' << std::flush;
}

const std::string acceptedEvent = R"({"protocol":1,"type":"accepted","jobId":"job1","device":"cpu"})";
const std::string cancelledEvent = R"({"protocol":1,"type":"cancelled","reason":"requested"})";
const std::string resultEvent = R"({"protocol":1,"type":"result","cacheDir":"C:\cache\key","summary":{"bpm":120}})";

std::string stageEvent (const char* id, int index)
{
    return R"({"protocol":1,"type":"stage","stage":")" + std::string (id) + R"(","index":)" + std::to_string (index)
         + R"(,"count":6})";
}

std::string progressEvent (const char* id, double fraction)
{
    return R"({"protocol":1,"type":"progress","stage":")" + std::string (id) + R"(","fraction":)"
         + std::to_string (fraction) + "}";
}

void emitNormalRun()
{
    emit (acceptedEvent);
    emit (stageEvent ("decoding", 1));
    emit (progressEvent ("decoding", 0.5));
    emit (stageEvent ("separating", 2));
    emit (progressEvent ("separating", 0.25));
    emit (resultEvent);
}

[[noreturn]] void quit (int code)
{
    std::cout << std::flush;
    std::cerr << std::flush;
    _exit (code); // not exit(): a thread may be blocked reading stdin
}

void sleepForever()
{
    juce::Thread::sleep (120000);
}

void spawnGrandchild (const std::string& exe)
{
    const auto pidFile = juce::SystemStats::getEnvironmentVariable ("SNDS_FAKE_PIDFILE", {});
    std::wstring commandLine = L"\"" + std::wstring (exe.begin(), exe.end()) + L"\" -I -m snds_worker hang-child";
    STARTUPINFOW startup {};
    startup.cb = sizeof (startup);
    startup.dwFlags = STARTF_USESTDHANDLES; // the grandchild holds the worker's pipes open
    startup.hStdInput = GetStdHandle (STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle (STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle (STD_ERROR_HANDLE);
    PROCESS_INFORMATION info {};

    if (CreateProcessW (
            nullptr, commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info))
    {
        std::ofstream (pidFile.toStdString()) << info.dwProcessId << std::endl;
        CloseHandle (info.hThread);
        CloseHandle (info.hProcess);
    }
}

// Behaves like the real worker for the session tests: reads the request, reports stages, and
// publishes a minimal valid package at the requested cacheDir before the result.
int runFakePublish (const std::string& request, const std::string& mode)
{
    const auto parsed = juce::JSON::parse (juce::String (request));
    const auto source = parsed["source"];
    const juce::File cacheDir (parsed["cacheDir"].toString());
    const juce::File jobDir (parsed["jobDir"].toString());
    const auto device = parsed["compute"]["preference"].toString() == "cpu" ? "cpu" : "cuda";

    emit (R"({"protocol":1,"type":"accepted","jobId":")" + parsed["jobId"].toString().toStdString() + R"(","device":")"
          + device + R"("})");
    emit (stageEvent ("decoding", 1));
    emit (stageEvent ("separating", 2));
    emit (progressEvent ("separating", 0.3));

    if (mode == "cuda-fail")
    {
        emit (
            R"({"protocol":1,"type":"failure","code":"cuda_failed","message":"CUDA error.","retryable":true,"suggestedAction":"retry_cpu"})");
        return 13;
    }

    if (mode == "disk-full")
    {
        emit (
            R"({"protocol":1,"type":"failure","code":"disk_full","message":"No space left on the device.","retryable":false,"suggestedAction":"free_disk_space"})");
        return 13;
    }

    if (mode == "conflict")
    {
        cacheDir.createDirectory();
        cacheDir.getChildFile ("song.json").replaceWithText ("{ broken");
        emit (
            R"({"protocol":1,"type":"failure","code":"cache_conflict","message":"The cache folder exists.","details":"corrupt","retryable":false})");
        return 13;
    }

    juce::Thread::sleep (150);
    emit (stageEvent ("normalizing", 3));
    emit (stageEvent ("analyzing_bpm", 4));
    emit (stageEvent ("validating", 5));
    emit (stageEvent ("finalizing", 6));

    snds::test::SongFixture fixture;
    fixture.sha256 = source["sha256"].toString();
    fixture.sizeBytes = static_cast<juce::int64> (source["sizeBytes"]);
    fixture.write (cacheDir.getParentDirectory(),
                   [&] (juce::DynamicObject& document)
                   {
                       document.getProperty ("worker").getDynamicObject()->setProperty ("device",
                                                                                        juce::String (device));
                       document.getProperty ("source").getDynamicObject()->setProperty ("path", source["path"]);
                       document.getProperty ("source").getDynamicObject()->setProperty (
                           "fileName", juce::File (source["path"].toString()).getFileName());
                   });

    // The real worker moves the staged artwork into the package and leaves the folder empty.
    for (const auto& file : jobDir.findChildFiles (juce::File::findFiles, false))
        file.deleteFile();

    emit (resultEvent);
    return 0;
}

int runFakePrepare (const char* executable)
{
    std::string request;
    std::getline (std::cin, request);

    const auto mode = juce::SystemStats::getEnvironmentVariable ("SNDS_FAKE_WORKER", {}).toStdString();
    std::cerr << "fake worker stderr line, mode " << mode << ", env "
              << juce::SystemStats::getEnvironmentVariable ("SNDS_FAKE_MARKER", "none") << std::endl;

    if (mode == "ok")
    {
        emitNormalRun();
        return 0;
    }

    if (mode == "publish" || mode == "cuda-fail" || mode == "disk-full" || mode == "conflict")
        return runFakePublish (request, mode);

    if (mode == "fail")
    {
        emit (acceptedEvent);
        emit (stageEvent ("decoding", 1));
        emit (
            R"({"protocol":1,"type":"failure","code":"decode_failed","message":"The file is damaged.","details":"trace","retryable":false})");
        return 13;
    }

    if (mode == "messy")
    {
        emit ("this is not json");
        emit ("");
        emit ("[1,2,3]");
        emit (R"({"protocol":1,"type":"telemetry","load":0.3})");
        emitNormalRun();
        return 0;
    }

    if (mode == "protocol2")
    {
        emit (acceptedEvent);
        emit (R"({"protocol":2,"type":"stage","stage":"decoding","index":1,"count":6})");
        sleepForever();
        return 0;
    }

    if (mode == "crash")
    {
        emit (acceptedEvent);
        TerminateProcess (GetCurrentProcess(), 0xC0000005);
        return 1;
    }

    if (mode == "silent-exit")
    {
        emit (acceptedEvent);
        return 0;
    }

    if (mode == "hang" || mode == "grandchild")
    {
        if (mode == "grandchild")
            spawnGrandchild (executable);

        emit (acceptedEvent);
        emit (stageEvent ("decoding", 1));
        sleepForever(); // ignores the cancel line and stdin closing
        return 0;
    }

    if (mode == "cooperative")
    {
        std::atomic<bool> stop { false };
        std::thread (
            [&stop]
            {
                std::string line;
                while (std::getline (std::cin, line))
                {
                    if (line.find ("cancel") != std::string::npos)
                        break;
                }
                stop = true;
            })
            .detach();

        emit (acceptedEvent);
        emit (stageEvent ("decoding", 1));

        while (! stop)
        {
            emit (progressEvent ("decoding", 0.1));
            juce::Thread::sleep (20);
        }

        emit (cancelledEvent);
        quit (12);
    }

    if (mode == "flood")
    {
        emit (acceptedEvent);
        emit (stageEvent ("separating", 2));

        // The test run's SNDS_HEAVY_TESTS reaches this child through the inherited environment.
        const int events = juce::SystemStats::getEnvironmentVariable ("SNDS_HEAVY_TESTS", {}) == "1" ? 20000 : 4000;

        for (int i = 0; i < events; ++i)
        {
            emit (progressEvent ("separating", i / static_cast<double> (events)));

            if (i % 10 == 0)
                std::cerr << "noise " << i << " ......................................................" << '\n';
        }

        emit (resultEvent);
        return 0;
    }

    return 2;
}

bool isFakeWorkerInvocation (int argc, char* argv[])
{
    // python -I -m snds_worker readiness
    return argc == 5 && std::string_view (argv[3]) == "snds_worker";
}
} // namespace

int main (int argc, char* argv[])
{
    if (isFakeWorkerInvocation (argc, argv))
    {
        const std::string_view command (argv[4]);

        if (command == "prepare")
            return runFakePrepare (argv[0]);

        if (command == "hang-child")
        {
            sleepForever();
            return 0;
        }

        return runFakeWorker();
    }

    ConsoleTestRunner runner;
    runner.setAssertOnFailure (false);
    // --e2e runs the opt-in checks against the real worker (see EndToEndTests.cpp).
    // --filter <text> runs only the default-category tests whose name contains <text>.
    // --shard <i>/<n> runs every n-th default-category test (i from 0), so n processes can share the suite.
    const bool endToEnd = argc == 2 && std::string_view (argv[1]) == "--e2e";
    const bool filtered = argc == 3 && std::string_view (argv[1]) == "--filter";
    const bool sharded = argc == 3 && std::string_view (argv[1]) == "--shard";

    if (filtered)
    {
        const juce::String needle (argv[2]);
        juce::Array<juce::UnitTest*> selected;

        for (auto* test : juce::UnitTest::getTestsInCategory ("snds"))
            if (test->getName().containsIgnoreCase (needle))
                selected.add (test);

        runner.runTests (selected);
    }
    else if (sharded)
    {
        const juce::String spec (argv[2]);
        const auto index = spec.upToFirstOccurrenceOf ("/", false, false).getIntValue();
        const auto count = juce::jmax (1, spec.fromFirstOccurrenceOf ("/", false, false).getIntValue());

        // The slowest suites are dealt out first, one per shard, so no shard gets two of them; the rest follow
        // in name order. (Measured with SNDS_TEST_TIMES=1; a stale list only unbalances the shards.)
        const juce::StringArray slowest { "Song session",
                                          "ValidateMemory",
                                          "OriginalAlignment",
                                          "PlaybackController",
                                          "Visual validation: renderer failure and contention",
                                          "Visual validation: mappings",
                                          "Fixture: owner music excerpt",
                                          "Worker process",
                                          "DSP mix processor",
                                          "Artwork",
                                          "Transport",
                                          "ValidateLongSong" };
        auto tests = juce::UnitTest::getTestsInCategory ("snds");
        const auto rank = [&] (juce::UnitTest* test)
        {
            const auto at = slowest.indexOf (test->getName());
            return at >= 0 ? at : slowest.size();
        };
        std::sort (tests.begin(),
                   tests.end(),
                   [&] (auto* a, auto* b)
                   { return rank (a) != rank (b) ? rank (a) < rank (b) : a->getName() < b->getName(); });
        juce::Array<juce::UnitTest*> selected;

        for (int i = 0; i < tests.size(); ++i)
            if (i % count == index)
                selected.add (tests[i]);

        runner.runTests (selected);
    }
    else
    {
        runner.runTestsInCategory (endToEnd ? "snds-e2e" : "snds");
    }

    int passes = 0;
    int failures = 0;

    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        passes += runner.getResult (i)->passes;
        failures += runner.getResult (i)->failures;
    }

    if (juce::SystemStats::getEnvironmentVariable ("SNDS_TEST_TIMES", {}) == "1")
        runner.printTimes();

    std::cout << "snds_tests: " << passes << " passed, " << failures << " failed" << std::endl;
    return (failures == 0 && passes > 0) ? 0 : 1;
}
