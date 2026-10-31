#pragma once

#include <juce_core/juce_core.h>

#include "app/WorkerLocation.h"

#include <functional>

namespace snds
{
/** What happened when the app ran `python -I -m snds_worker readiness`. */
struct ReadinessRun
{
    enum class Outcome
    {
        finished,
        environmentMissing,
        failedToStart,
        timedOut,
        cancelled
    };

    Outcome outcome = Outcome::finished;
    int exitCode = -1;   // only meaningful when finished
    juce::String output; // stdout only; the worker's stderr is not read
    double seconds = 0.0;
};

/** Runs the readiness command and collects its stdout. Blocks until the process exits, the
    timeout passes, or shouldCancel() returns true (polled every 50 ms); in the last two cases the
    process is killed. Safe to call from any thread.
*/
ReadinessRun runReadinessCommand (const WorkerLocation& location,
                                  int timeoutMs,
                                  std::function<bool()> shouldCancel = {});

/** What the app shows about the worker. */
struct Readiness
{
    enum class State
    {
        checking,
        ready,
        notSetUp,
        modelMissing,
        error
    };

    State state = State::checking;
    juce::String headline; // one line
    juce::String guidance; // what to do next; empty when ready or checking
    juce::String details;  // multi-line: shown on request and written to the log

    bool isReady() const noexcept { return state == State::ready; }

    /** "checking", "ready", "not-set-up", "model-missing", or "error". */
    static juce::String stateName (State state);
};

/** Maps a run to what the app shows. The JSON report on stdout is trusted over the exit code,
    which is 1 whenever the worker is not ready.
*/
Readiness interpretReadiness (const ReadinessRun& run, const WorkerLocation& location);

/** Runs one readiness check on a background thread and calls onFinished on that thread.
    Destroying the checker cancels a check in progress and kills the worker process.
*/
class ReadinessChecker final : private juce::Thread
{
public:
    using Callback = std::function<void (const Readiness&)>;

    ReadinessChecker (WorkerLocation location, int timeoutMs, Callback onFinished);
    ~ReadinessChecker() override;

private:
    void run() override;

    const WorkerLocation location;
    const int timeoutMs;
    const Callback onFinished;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReadinessChecker)
};
} // namespace snds
