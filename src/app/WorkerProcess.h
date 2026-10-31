#pragma once

#include <juce_core/juce_core.h>

#include "app/WorkerLocation.h"
#include "model/WorkerProtocol.h"

#include <functional>
#include <memory>
#include <optional>
#include <thread>

namespace snds
{
/** How a `prepare` job ended, as the app sees it. */
struct WorkerJobResult
{
    bool launched = false;
    juce::String launchError; // set when launched is false

    int exitCode = -1;
    worker::TerminalKind terminalKind = worker::TerminalKind::none;

    /** The result, failure, or cancelled event that ended the job, if the worker sent one. */
    std::optional<worker::Event> terminalEvent;

    worker::JobOutcome outcome = worker::JobOutcome::crashed;

    bool cancelRequested = false; // the caller asked for cancel (or the destructor did)
    bool timedOut = false;        // the overall timeout passed and the job was cancelled
    bool killedByApp = false;     // the app terminated the job tree

    /** Set when the worker spoke an unsupported protocol; the job was killed. */
    juce::String protocolError;

    int invalidLines = 0;  // stdout lines that were not usable events
    int unknownEvents = 0; // well-formed events of a type this app does not know
    int deletedAudioFiles = 0;

    juce::String stderrTail; // the end of the worker's stderr, redacted, for diagnostics
    double seconds = 0.0;
};

struct WorkerProcessOptions
{
    juce::StringArray commandLine; // [0] is the executable; the rest are its arguments
    juce::File workingDirectory;
    juce::String requestLine; // the first stdin line, without a newline
    juce::File jobDir;        // created if needed; gets worker-stderr.log; cleaned of audio on failure

    /** Added to (or replacing) the parent's environment for the child. */
    juce::StringPairArray extraEnvironment;

    /** After a cancel request the worker gets this long to acknowledge before the whole process
        tree is terminated. */
    int cancelGraceMs = 5000;

    /** Cancels the job (gracefully, then by force) after this long. 0 means no limit. */
    int timeoutMs = 0;
};

/** Runs one worker job as a child process tree inside a Win32 Job Object.

    Launch: CreateProcessW suspended with redirected stdin, stdout, and stderr pipes, assigned
    to a job with KILL_ON_JOB_CLOSE before it runs, so no descendant (uv's launcher python.exe
    starts the real interpreter) can escape and the tree dies with the app.

    Threads: a supervisor thread does the launching and waiting, one thread reads stdout (NDJSON
    lines become worker::Event values), one drains stderr into the app log and
    jobDir\worker-stderr.log. Nothing here blocks the message thread except the destructor, which
    terminates the tree and joins. Callbacks are delivered through the dispatcher (by default the
    message thread), in order, and never after the destructor has run. onFinished comes last.

    Cancel: the caller's cancel() sends the cancel line and closes stdin; if the worker has not
    exited after cancelGraceMs, the job is terminated with exit code 99. The same sequence
    handles the overall timeout. A worker that exits leaves no descendants: the job is terminated
    once the main process is gone.

    After any outcome except success, audio files left in jobDir are deleted; logs stay.
*/
class WorkerProcess
{
public:
    using Dispatcher = std::function<void (std::function<void()>)>;

    struct Callbacks
    {
        /** accepted, stage, progress, cancelled, failure, result. Unknown and unparsable lines
            are counted and logged, not delivered. */
        std::function<void (const worker::Event&)> onEvent;
        std::function<void (const WorkerJobResult&)> onFinished;
    };

    /** An empty dispatcher posts to the JUCE message thread. */
    WorkerProcess (WorkerProcessOptions options, Callbacks callbacks, Dispatcher dispatcher = {});

    /** Terminates a job still running and waits for the threads. */
    ~WorkerProcess();

    /** Starts the job. Returns immediately; launch problems arrive through onFinished. */
    void start();

    /** Asks for a graceful cancel. Thread-safe and non-blocking. */
    void cancel();

    /** True from start() until the job's threads have finished. Thread-safe. */
    bool isRunning() const;

    /** The command for `python -I -m snds_worker prepare` with the request on stdin. */
    static WorkerProcessOptions makePrepareOptions (const WorkerLocation& location,
                                                    const worker::PrepareRequest& request);

private:
    struct Shared;
    std::shared_ptr<Shared> shared;
    std::thread supervisor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WorkerProcess)
};

/** Deletes audio files (and other bulky intermediates) under jobDir, then empty folders, and
    leaves logs and requests. Returns how many files were deleted.
*/
int deleteIncompleteAudio (const juce::File& jobDir);

/** Quotes one argument for a Windows command line (CommandLineToArgvW rules). */
juce::String quoteCommandLineArgument (const juce::String& argument);
} // namespace snds
