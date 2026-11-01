#pragma once

#include <juce_core/juce_core.h>

#include <optional>
#include <variant>

namespace snds::worker
{
/** The job protocol version this app speaks. See docs/protocol.md. */
constexpr int protocolVersion = 1;

/** Exit codes of a `prepare` run. 99 is assigned by the app when it kills the job object. */
namespace exitcode
{
constexpr int ok = 0;
constexpr int invalidRequest = 10;
constexpr int unsupportedProtocol = 11;
constexpr int cancelled = 12;
constexpr int jobFailed = 13;
constexpr int internalError = 14;
constexpr int killedByApp = 99;
} // namespace exitcode

/** The prepare stages, in the order the worker reports them. */
enum class Stage
{
    decoding,
    separating,
    normalizing,
    analyzingBpm,
    validating,
    finalizing
};

constexpr int stageCount = 6;

/** The wire id: "decoding", "separating", "normalizing", "analyzing_bpm", "validating", "finalizing". */
juce::String stageId (Stage stage);
std::optional<Stage> stageFromId (const juce::String& id);

// --- Events from the worker ---------------------------------------------------------------

struct AcceptedEvent
{
    juce::String jobId;
    juce::String device; // "cuda" or "cpu"
};

struct StageEvent
{
    Stage stage = Stage::decoding;
    int index = 0; // 1-based
    int count = stageCount;
};

struct ProgressEvent
{
    Stage stage = Stage::decoding;
    double fraction = 0.0;            // clamped to 0..1
    std::optional<double> etaSeconds; // present only when the worker considers it credible
};

struct CancelledEvent
{
    juce::String reason; // may be empty
};

struct FailureEvent
{
    juce::String code; // stable machine-readable id; unknown codes are treated as generic
    juce::String message;
    juce::String details; // diagnostic text for the log; may be empty
    bool retryable = false;
    juce::String suggestedAction; // for example "retry_cpu"; may be empty
};

struct ResultEvent
{
    juce::String cacheDir;
    juce::var summary;
};

/** A well-formed event of a type this app does not know. Ignore it and log it. */
struct UnknownEvent
{
    juce::String type;
};

/** A line that is not a usable event: not JSON, not an object, missing or wrongly typed fields. */
struct InvalidLine
{
    juce::String reason;
};

/** A well-formed event whose `protocol` this app does not support. Ends the job as an error. */
struct UnsupportedProtocolLine
{
    juce::var protocol; // as sent; may be a number or something else entirely
};

using Event = std::variant<AcceptedEvent,
                           StageEvent,
                           ProgressEvent,
                           CancelledEvent,
                           FailureEvent,
                           ResultEvent,
                           UnknownEvent,
                           InvalidLine,
                           UnsupportedProtocolLine>;

/** Parses one stdout line. Never throws; problems come back as InvalidLine or
    UnsupportedProtocolLine. Fields the app does not know are ignored.
*/
Event parseEventLine (const juce::String& line);

/** True for result, failure, and cancelled: the events that end a job. */
bool isTerminal (const Event& event);

// --- Messages to the worker ---------------------------------------------------------------

struct PrepareRequest
{
    juce::String jobId;

    juce::File sourceFile;
    juce::int64 sourceSizeBytes = 0;
    juce::String sourceSha256; // 64 lowercase hex characters

    juce::File jobDir;   // temporary work directory under SNDS_HOME\jobs
    juce::File cacheDir; // final package location under SNDS_HOME\cache

    bool forceCpu = false; // compute.preference "cpu" instead of "auto"

    struct Artwork
    {
        juce::File file;     // an image the app wrote into jobDir
        juce::String origin; // "embedded", "file", or "fallback"
    };

    std::optional<Artwork> artwork;
};

/** The one-line, pure-ASCII JSON request, without a trailing newline. */
juce::String buildPrepareRequest (const PrepareRequest& request);

/** The control line that asks the worker to cancel gracefully, without a trailing newline. */
juce::String buildCancelLine();

// --- Exit codes ---------------------------------------------------------------------------

/** What an exit code means on its own. */
enum class ExitMeaning
{
    ok,
    invalidRequest,
    unsupportedProtocol,
    cancelled,
    jobFailed,
    internalError,
    killedByApp,
    crashed // any code outside the table
};

ExitMeaning classifyExitCode (int exitCode);

/** Which terminal event the app saw on stdout, if any. */
enum class TerminalKind
{
    none,
    result,
    failure,
    cancelled
};

TerminalKind terminalKind (const Event& event);

/** How a finished job ends, from the exit code and the terminal event together. */
enum class JobOutcome
{
    succeeded,
    failed, // the worker reported a failure event and exited consistently
    cancelled,
    killed,           // the app terminated the job (exit 99)
    crashed,          // exit code outside the table, whatever was on stdout
    protocolViolation // exit code and terminal event disagree, or no terminal event
};

JobOutcome resolveJobOutcome (int exitCode, TerminalKind terminal);

/** A short phrase for logs and messages. */
juce::String describe (JobOutcome outcome);
} // namespace snds::worker
