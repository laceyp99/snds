#include "app/Readiness.h"

#include <atomic>
#include <thread>

namespace snds
{
namespace
{
constexpr auto bootstrapGuidance = "Run scripts\\bootstrap.ps1, then press Re-check.";
constexpr auto modelGuidance = "Run scripts\\bootstrap.ps1 to download it, then press Re-check.";
constexpr auto diagnosticsGuidance = "Run scripts\\diagnostics.ps1 for a full report.";

const juce::StringArray modelProblems { "model_missing",           "model_size_mismatch", "model_unreadable",
                                        "model_hash_mismatch",     "bpm_model_missing",   "bpm_model_size_mismatch",
                                        "bpm_model_hash_mismatch", "bpm_model_unreadable" };

double millisecondsNow()
{
    return juce::Time::getMillisecondCounterHiRes();
}

// The worker prints exactly one JSON object; tolerate stray lines around it anyway.
juce::var findReport (const juce::String& output)
{
    auto lines = juce::StringArray::fromLines (output);
    lines.trim();
    lines.removeEmptyStrings();

    for (auto index = lines.size(); --index >= 0;)
    {
        juce::var parsed;

        if (juce::JSON::parse (lines[index], parsed).wasOk() && parsed.getDynamicObject() != nullptr)
            return parsed;
    }

    return {};
}

juce::String indent (const juce::String& text)
{
    auto lines = juce::StringArray::fromLines (text.trimEnd());

    for (auto& line : lines)
        line = "    " + line;

    return lines.joinIntoString ("\n");
}

juce::String firstLine (const juce::String& text)
{
    const auto line = text.upToFirstOccurrenceOf ("\n", false, false).trim();
    return line.length() > 200 ? line.substring (0, 200) + "..." : line;
}

juce::String describeRun (const ReadinessRun& run, const WorkerLocation& location)
{
    juce::StringArray lines;
    lines.add ("Worker folder: " + location.root.getFullPathName() + " (" + location.source + ")");
    lines.add ("Python: " + location.pythonExecutable().getFullPathName());

    if (run.outcome == ReadinessRun::Outcome::finished)
        lines.add ("Exit code " + juce::String (run.exitCode) + " after " + juce::String (run.seconds, 1) + " s");

    return lines.joinIntoString ("\n");
}

juce::String describeReport (const juce::var& report)
{
    juce::StringArray lines;

    if (const auto* problems = report["problems"].getArray(); problems != nullptr && ! problems->isEmpty())
    {
        lines.add ("Problems:");

        for (const auto& problem : *problems)
        {
            lines.add ("  [" + problem["code"].toString() + "] " + problem["message"].toString());

            if (const auto details = problem["details"].toString(); details.isNotEmpty())
                lines.add (indent (details));
        }
    }

    if (auto* packages = report["packages"].getDynamicObject())
    {
        juce::StringArray versions;

        for (const auto& package : packages->getProperties())
            versions.add (package.name.toString() + " "
                          + (package.value.isVoid() ? juce::String ("missing") : package.value.toString()));

        lines.add ("Python " + report["worker"]["python"].toString() + "; " + versions.joinIntoString (", "));
    }

    if (const auto compute = report["compute"]; compute.isObject())
    {
        auto line = "PyTorch build: " + compute["torch_build"].toString() + "; device: " + compute["device"].toString();

        if (compute["cuda_device"].toString().isNotEmpty())
            line << " (" << compute["cuda_device"].toString() << ")";

        if (compute["cuda_error"].toString().isNotEmpty())
            line << "; CUDA error: " << compute["cuda_error"].toString();

        lines.add (line);
    }

    if (const auto model = report["model"]; model.isObject())
        lines.add ("Model: " + model["path"].toString() + " ("
                   + (static_cast<bool> (model["present"]) ? model["size_bytes"].toString() + " bytes"
                                                           : juce::String ("missing"))
                   + ")");

    return lines.joinIntoString ("\n");
}

Readiness make (Readiness::State state,
                const juce::String& headline,
                const juce::String& guidance,
                const juce::String& details)
{
    return { state, headline, guidance, details };
}

Readiness interpretReport (const juce::var& report, const juce::String& context)
{
    const auto details = context + "\n" + describeReport (report);

    if (static_cast<int> (report["protocol"]) != 1)
        return make (Readiness::State::error,
                     "The worker uses protocol version " + report["protocol"].toString()
                         + "; this app needs version 1.",
                     diagnosticsGuidance,
                     details);

    juce::StringArray codes;
    juce::String firstOtherProblem;

    if (const auto* problems = report["problems"].getArray())
    {
        for (const auto& problem : *problems)
        {
            const auto code = problem["code"].toString();
            codes.add (code);

            if (firstOtherProblem.isEmpty() && code != "package_missing" && ! modelProblems.contains (code))
                firstOtherProblem = firstLine (problem["message"].toString());
        }
    }

    if (report["type"].toString() != "readiness")
        return make (Readiness::State::error,
                     "The readiness check failed inside the worker"
                         + (firstOtherProblem.isNotEmpty() ? ": " + firstOtherProblem : juce::String (".")),
                     diagnosticsGuidance,
                     details);

    if (codes.isEmpty())
    {
        if (! static_cast<bool> (report["ready"]))
            return make (Readiness::State::error,
                         "The worker reported that it is not ready without saying why.",
                         diagnosticsGuidance,
                         details);

        const auto compute = report["compute"];
        juce::String headline ("Worker ready: CPU");

        if (compute["device"].toString() == "cuda")
            headline = "Worker ready: CUDA on "
                     + (compute["cuda_device"].toString().isNotEmpty() ? compute["cuda_device"].toString()
                                                                       : juce::String ("an NVIDIA GPU"));
        else if (compute["torch_build"].toString() == "cuda")
            headline = "Worker ready: CPU (no usable NVIDIA GPU)";

        return make (Readiness::State::ready, headline, {}, details);
    }

    if (firstOtherProblem.isNotEmpty())
        return make (Readiness::State::error, firstOtherProblem, diagnosticsGuidance, details);

    if (codes.contains ("package_missing"))
        return make (Readiness::State::notSetUp, "The worker environment is incomplete.", bootstrapGuidance, details);

    const char* headline = nullptr;

    if (codes.contains ("model_missing"))
        headline = "The separation model is not installed.";
    else if (codes.contains ("bpm_model_missing"))
        headline = "The beat-detection model is not installed.";
    else if (codes.contains ("model_size_mismatch") || codes.contains ("model_hash_mismatch")
             || codes.contains ("model_unreadable"))
        headline = "The separation model file is damaged or incomplete.";
    else
        headline = "The beat-detection model file is damaged or incomplete.";

    return make (Readiness::State::modelMissing, headline, modelGuidance, details);
}
} // namespace

//==============================================================================
ReadinessRun runReadinessCommand (const WorkerLocation& location, int timeoutMs, std::function<bool()> shouldCancel)
{
    const auto started = millisecondsNow();
    ReadinessRun run;

    const auto finish = [&] (ReadinessRun::Outcome outcome)
    {
        run.outcome = outcome;
        run.seconds = (millisecondsNow() - started) / 1000.0;
        return run;
    };

    if (! location.pythonExecutable().existsAsFile())
        return finish (ReadinessRun::Outcome::environmentMissing);

    // Only stdout is piped: the protocol puts exactly one JSON object there and everything else
    // on stderr, which juce::ChildProcess then leaves unconnected.
    juce::ChildProcess process;

    if (! process.start (location.commandFor ("readiness"), juce::ChildProcess::wantStdOut))
        return finish (ReadinessRun::Outcome::failedToStart);

    // readAllProcessOutput() blocks until the process ends, so a watchdog enforces the timeout
    // and cancellation by killing the process, which ends the read.
    enum class Stop
    {
        none,
        timeout,
        cancel
    };
    std::atomic<Stop> stop { Stop::none };
    juce::WaitableEvent readFinished;

    std::thread watchdog (
        [&]
        {
            while (! readFinished.wait (50))
            {
                if (shouldCancel && shouldCancel())
                    stop = Stop::cancel;
                else if (millisecondsNow() - started >= timeoutMs)
                    stop = Stop::timeout;
                else
                    continue;

                process.kill();
                return;
            }
        });

    run.output = process.readAllProcessOutput();
    readFinished.signal();
    watchdog.join();

    if (stop == Stop::timeout)
        return finish (ReadinessRun::Outcome::timedOut);

    if (stop == Stop::cancel)
        return finish (ReadinessRun::Outcome::cancelled);

    process.waitForProcessToFinish (5000);
    run.exitCode = static_cast<int> (process.getExitCode());
    return finish (ReadinessRun::Outcome::finished);
}

//==============================================================================
juce::String Readiness::stateName (State state)
{
    switch (state)
    {
        case State::checking:
            return "checking";
        case State::ready:
            return "ready";
        case State::notSetUp:
            return "not-set-up";
        case State::modelMissing:
            return "model-missing";
        case State::error:
            return "error";
    }

    return "error";
}

Readiness interpretReadiness (const ReadinessRun& run, const WorkerLocation& location)
{
    const auto context = describeRun (run, location);

    switch (run.outcome)
    {
        case ReadinessRun::Outcome::environmentMissing:
            return make (
                Readiness::State::notSetUp, "The worker environment is not set up.", bootstrapGuidance, context);

        case ReadinessRun::Outcome::failedToStart:
            return make (Readiness::State::error, "The worker could not be started.", diagnosticsGuidance, context);

        case ReadinessRun::Outcome::timedOut:
        {
            const auto seconds = juce::roundToInt (run.seconds);
            return make (Readiness::State::error,
                         "The readiness check did not finish within " + juce::String (seconds)
                             + (seconds == 1 ? " second." : " seconds."),
                         diagnosticsGuidance,
                         context);
        }

        case ReadinessRun::Outcome::cancelled:
            return make (Readiness::State::error, "The readiness check was cancelled.", {}, context);

        case ReadinessRun::Outcome::finished:
            break;
    }

    const auto report = findReport (run.output);

    if (report.isVoid())
    {
        const auto output = run.output.trim();
        return make (Readiness::State::error,
                     "The worker exited with code " + juce::String (run.exitCode) + " without a readiness report.",
                     diagnosticsGuidance,
                     context + "\nOutput:\n"
                         + (output.isEmpty() ? juce::String ("(none)") : indent (output.substring (0, 4000))));
    }

    return interpretReport (report, context);
}

//==============================================================================
ReadinessChecker::ReadinessChecker (WorkerLocation workerLocation, int timeout, Callback callback)
    : juce::Thread ("Readiness check"),
      location (std::move (workerLocation)),
      timeoutMs (timeout),
      onFinished (std::move (callback))
{
    startThread();
}

ReadinessChecker::~ReadinessChecker()
{
    // The watchdog notices within 50 ms and kills the worker, which ends the thread.
    stopThread (10000);
}

void ReadinessChecker::run()
{
    const auto result = runReadinessCommand (location, timeoutMs, [this] { return threadShouldExit(); });

    if (result.outcome != ReadinessRun::Outcome::cancelled && onFinished != nullptr)
        onFinished (interpretReadiness (result, location));
}
} // namespace snds
