#include "model/WorkerProtocol.h"

#include <algorithm>

namespace snds::worker
{
namespace
{
constexpr const char* stageIds[] {
    "decoding", "separating", "normalizing", "analyzing_bpm", "validating", "finalizing"
};

bool isRealObject (const juce::var& value)
{
    // var::isObject() is also true for arrays.
    return value.getDynamicObject() != nullptr;
}

bool isInteger (const juce::var& value)
{
    return value.isInt() || value.isInt64();
}

bool isNumber (const juce::var& value)
{
    return isInteger (value) || value.isDouble();
}

/** Reads fields of one event, remembering the first problem. */
class Fields
{
public:
    explicit Fields (const juce::DynamicObject& objectToRead)
        : object (objectToRead)
    {
    }

    juce::String text (const char* name, bool required)
    {
        const auto value = object.getProperty (name);

        if (value.isString())
            return value.toString();

        if (required || ! value.isVoid())
            fail (name, "a string");

        return {};
    }

    int integer (const char* name)
    {
        const auto value = object.getProperty (name);

        if (! isInteger (value))
        {
            fail (name, "an integer");
            return 0;
        }

        return (int) juce::jlimit<juce::int64> (0, 1000000, (juce::int64) value);
    }

    std::optional<double> number (const char* name, bool required)
    {
        const auto value = object.getProperty (name);

        if (isNumber (value))
            return (double) value;

        if (required || ! (value.isVoid() || value.isUndefined()))
            fail (name, "a number");

        return std::nullopt;
    }

    bool boolean (const char* name, bool fallback)
    {
        const auto value = object.getProperty (name);

        if (value.isBool())
            return (bool) value;

        if (! value.isVoid())
            fail (name, "a boolean");

        return fallback;
    }

    Stage stage()
    {
        const auto id = text ("stage", true);
        const auto parsed = stageFromId (id);

        if (parsed.has_value())
            return *parsed;

        if (problem.isEmpty())
            problem = "unknown stage \"" + id + "\"";

        return Stage::decoding;
    }

    bool ok() const noexcept { return problem.isEmpty(); }

    juce::String problem;

private:
    void fail (const char* name, const char* expected)
    {
        if (problem.isEmpty())
            problem = juce::String ("field \"") + name + "\" must be " + expected;
    }

    const juce::DynamicObject& object;
};

// JSON structure is ASCII, so every non-ASCII character is inside a string and can be escaped.
// juce::JSON writes such characters as they are.
juce::String escapeNonAscii (const juce::String& json)
{
    juce::String escaped;

    for (auto character : json)
    {
        if (character < 128)
        {
            escaped << juce::String::charToString (character);
            continue;
        }

        const auto escape = [&] (juce::uint32 unit)
        { escaped << "\\u" << juce::String::toHexString ((int) unit).paddedLeft ('0', 4); };

        if (character < 0x10000)
        {
            escape ((juce::uint32) character);
        }
        else
        {
            const auto offset = (juce::uint32) character - 0x10000;
            escape (0xd800 + (offset >> 10));
            escape (0xdc00 + (offset & 0x3ff));
        }
    }

    return escaped;
}

juce::String ellipsize (const juce::String& text)
{
    return text.length() > 120 ? text.substring (0, 120) + "..." : text;
}
} // namespace

juce::String stageId (Stage stage)
{
    return stageIds[(int) stage];
}

std::optional<Stage> stageFromId (const juce::String& id)
{
    for (int index = 0; index < stageCount; ++index)
        if (id == stageIds[index])
            return (Stage) index;

    return std::nullopt;
}

Event parseEventLine (const juce::String& line)
{
    const auto trimmed = line.trim();

    if (trimmed.isEmpty())
        return InvalidLine { "empty line" };

    juce::var parsed;

    if (juce::JSON::parse (trimmed, parsed).failed())
        return InvalidLine { "not valid JSON: " + ellipsize (trimmed) };

    auto* object = parsed.getDynamicObject();

    if (object == nullptr)
        return InvalidLine { "not a JSON object: " + ellipsize (trimmed) };

    const auto version = object->getProperty ("protocol");

    if (! isInteger (version))
        return InvalidLine { "field \"protocol\" must be an integer" };

    if ((juce::int64) version != protocolVersion)
        return UnsupportedProtocolLine { version };

    const auto typeValue = object->getProperty ("type");

    if (! typeValue.isString() || typeValue.toString().isEmpty())
        return InvalidLine { "field \"type\" must be a non-empty string" };

    const auto type = typeValue.toString();
    Fields fields (*object);

    const auto invalid = [&] (const juce::String& eventType) -> Event
    { return InvalidLine { "invalid " + eventType + " event: " + fields.problem }; };

    if (type == "accepted")
    {
        AcceptedEvent event;
        event.jobId = fields.text ("jobId", true);
        event.device = fields.text ("device", true);
        return fields.ok() ? Event (event) : invalid (type);
    }

    if (type == "stage")
    {
        StageEvent event;
        event.stage = fields.stage();
        event.index = fields.integer ("index");
        event.count = fields.integer ("count");
        return fields.ok() ? Event (event) : invalid (type);
    }

    if (type == "progress")
    {
        ProgressEvent event;
        event.stage = fields.stage();
        event.fraction = juce::jlimit (0.0, 1.0, fields.number ("fraction", true).value_or (0.0));
        event.etaSeconds = fields.number ("etaSeconds", false);

        if (event.etaSeconds.has_value())
            event.etaSeconds = std::max (0.0, *event.etaSeconds);

        return fields.ok() ? Event (event) : invalid (type);
    }

    if (type == "cancelled")
    {
        CancelledEvent event;
        event.reason = fields.text ("reason", false);
        return fields.ok() ? Event (event) : invalid (type);
    }

    if (type == "failure")
    {
        FailureEvent event;
        event.code = fields.text ("code", true);
        event.message = fields.text ("message", true);
        event.details = fields.text ("details", false);
        event.retryable = fields.boolean ("retryable", false);
        event.suggestedAction = fields.text ("suggestedAction", false);
        return fields.ok() ? Event (event) : invalid (type);
    }

    if (type == "result")
    {
        ResultEvent event;
        event.cacheDir = fields.text ("cacheDir", true);
        event.summary = object->getProperty ("summary");

        if (fields.ok() && ! event.summary.isVoid() && ! isRealObject (event.summary))
            fields.problem = "field \"summary\" must be an object";

        return fields.ok() ? Event (event) : invalid (type);
    }

    return UnknownEvent { type };
}

bool isTerminal (const Event& event)
{
    return terminalKind (event) != TerminalKind::none;
}

juce::String buildPrepareRequest (const PrepareRequest& request)
{
    auto* source = new juce::DynamicObject();
    source->setProperty ("path", request.sourceFile.getFullPathName());
    source->setProperty ("sizeBytes", request.sourceSizeBytes);
    source->setProperty ("sha256", request.sourceSha256);

    auto* compute = new juce::DynamicObject();
    compute->setProperty ("preference", request.forceCpu ? "cpu" : "auto");

    auto* root = new juce::DynamicObject();
    root->setProperty ("protocol", protocolVersion);
    root->setProperty ("type", "prepare");
    root->setProperty ("jobId", request.jobId);
    root->setProperty ("source", juce::var (source));
    root->setProperty ("jobDir", request.jobDir.getFullPathName());
    root->setProperty ("cacheDir", request.cacheDir.getFullPathName());
    root->setProperty ("compute", juce::var (compute));

    if (request.artwork.has_value())
    {
        auto* artwork = new juce::DynamicObject();
        artwork->setProperty ("path", request.artwork->file.getFullPathName());
        artwork->setProperty ("origin", request.artwork->origin);
        root->setProperty ("artwork", juce::var (artwork));
    }

    return escapeNonAscii (juce::JSON::toString (juce::var (root), true, 20));
}

juce::String buildCancelLine()
{
    return juce::String (R"({"protocol":1,"type":"cancel"})");
}

ExitMeaning classifyExitCode (int exitCode)
{
    switch (exitCode)
    {
        case exitcode::ok:
            return ExitMeaning::ok;
        case exitcode::invalidRequest:
            return ExitMeaning::invalidRequest;
        case exitcode::unsupportedProtocol:
            return ExitMeaning::unsupportedProtocol;
        case exitcode::cancelled:
            return ExitMeaning::cancelled;
        case exitcode::jobFailed:
            return ExitMeaning::jobFailed;
        case exitcode::internalError:
            return ExitMeaning::internalError;
        case exitcode::killedByApp:
            return ExitMeaning::killedByApp;
        default:
            return ExitMeaning::crashed;
    }
}

TerminalKind terminalKind (const Event& event)
{
    if (std::holds_alternative<ResultEvent> (event))
        return TerminalKind::result;

    if (std::holds_alternative<FailureEvent> (event))
        return TerminalKind::failure;

    if (std::holds_alternative<CancelledEvent> (event))
        return TerminalKind::cancelled;

    return TerminalKind::none;
}

JobOutcome resolveJobOutcome (int exitCode, TerminalKind terminal)
{
    const auto meaning = classifyExitCode (exitCode);

    if (meaning == ExitMeaning::killedByApp)
        return JobOutcome::killed;

    if (meaning == ExitMeaning::crashed)
        return JobOutcome::crashed;

    switch (terminal)
    {
        case TerminalKind::result:
            return meaning == ExitMeaning::ok ? JobOutcome::succeeded : JobOutcome::protocolViolation;

        case TerminalKind::cancelled:
            return meaning == ExitMeaning::cancelled ? JobOutcome::cancelled : JobOutcome::protocolViolation;

        case TerminalKind::failure:
            return meaning == ExitMeaning::ok || meaning == ExitMeaning::cancelled ? JobOutcome::protocolViolation
                                                                                   : JobOutcome::failed;

        case TerminalKind::none:
            break;
    }

    return JobOutcome::protocolViolation;
}

juce::String describe (JobOutcome outcome)
{
    switch (outcome)
    {
        case JobOutcome::succeeded:
            return "succeeded";
        case JobOutcome::failed:
            return "failed";
        case JobOutcome::cancelled:
            return "cancelled";
        case JobOutcome::killed:
            return "killed by the app";
        case JobOutcome::crashed:
            return "crashed";
        case JobOutcome::protocolViolation:
            return "ended without a consistent result (protocol violation)";
    }

    return {};
}
} // namespace snds::worker
