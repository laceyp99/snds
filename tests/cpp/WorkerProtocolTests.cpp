#include <juce_core/juce_core.h>

#include "model/WorkerProtocol.h"

namespace snds::worker
{
namespace
{
class WorkerProtocolTests final : public juce::UnitTest
{
public:
    WorkerProtocolTests()
        : juce::UnitTest ("WorkerProtocol", "snds")
    {
    }

    void runTest() override
    {
        parsesEveryEventType();
        toleratesUnknownAndExtraContent();
        rejectsMalformedLines();
        rejectsUnsupportedVersions();
        buildsRequests();
        mapsExitCodes();
    }

private:
    template <typename T>
    const T* as (const Event& event)
    {
        return std::get_if<T> (&event);
    }

    void parsesEveryEventType()
    {
        beginTest ("accepted");
        {
            const auto event =
                parseEventLine (R"({"protocol":1,"type":"accepted","jobId":"j1","device":"cuda","seq":1})");
            const auto* accepted = as<AcceptedEvent> (event);
            expect (accepted != nullptr);

            if (accepted != nullptr)
            {
                expectEquals (accepted->jobId, juce::String ("j1"));
                expectEquals (accepted->device, juce::String ("cuda"));
            }

            expect (! isTerminal (event));
        }

        beginTest ("stage ids, index and count");
        {
            const auto event =
                parseEventLine (R"({"protocol":1,"type":"stage","stage":"analyzing_bpm","index":4,"count":6})");
            const auto* stage = as<StageEvent> (event);
            expect (stage != nullptr);

            if (stage != nullptr)
            {
                expect (stage->stage == Stage::analyzingBpm);
                expectEquals (stage->index, 4);
                expectEquals (stage->count, 6);
            }

            for (int index = 0; index < stageCount; ++index)
            {
                const auto each = (Stage) index;
                expect (stageFromId (stageId (each)) == each);
            }

            expect (! stageFromId ("mixing").has_value());
            expectEquals (stageId (Stage::finalizing), juce::String ("finalizing"));
        }

        beginTest ("progress with and without an ETA, clamped");
        {
            const auto withEta = parseEventLine (
                R"({"protocol":1,"type":"progress","stage":"separating","fraction":0.5,"etaSeconds":42})");
            const auto* progress = as<ProgressEvent> (withEta);
            expect (progress != nullptr);

            if (progress != nullptr)
            {
                expect (progress->stage == Stage::separating);
                expectWithinAbsoluteError (progress->fraction, 0.5, 1.0e-9);
                expect (progress->etaSeconds.has_value());
                expectWithinAbsoluteError (progress->etaSeconds.value_or (0.0), 42.0, 1.0e-9);
            }

            const auto bare = parseEventLine (R"({"protocol":1,"type":"progress","stage":"decoding","fraction":1.7})");
            const auto* clamped = as<ProgressEvent> (bare);
            expect (clamped != nullptr);

            if (clamped != nullptr)
            {
                expectWithinAbsoluteError (clamped->fraction, 1.0, 1.0e-9);
                expect (! clamped->etaSeconds.has_value());
            }
        }

        beginTest ("cancelled");
        {
            const auto event = parseEventLine (R"({"protocol":1,"type":"cancelled","reason":"stdin_closed"})");
            const auto* cancelled = as<CancelledEvent> (event);
            expect (cancelled != nullptr);
            expect (isTerminal (event));
            expect (terminalKind (event) == TerminalKind::cancelled);

            if (cancelled != nullptr)
                expectEquals (cancelled->reason, juce::String ("stdin_closed"));

            expect (as<CancelledEvent> (parseEventLine (R"({"protocol":1,"type":"cancelled"})")) != nullptr);
        }

        beginTest ("failure");
        {
            const auto event = parseEventLine (
                R"({"protocol":1,"type":"failure","code":"cuda_failed","message":"CUDA broke","details":"trace","retryable":true,"suggestedAction":"retry_cpu"})");
            const auto* failure = as<FailureEvent> (event);
            expect (failure != nullptr);
            expect (terminalKind (event) == TerminalKind::failure);

            if (failure != nullptr)
            {
                expectEquals (failure->code, juce::String ("cuda_failed"));
                expectEquals (failure->message, juce::String ("CUDA broke"));
                expectEquals (failure->details, juce::String ("trace"));
                expect (failure->retryable);
                expectEquals (failure->suggestedAction, juce::String ("retry_cpu"));
            }

            const auto minimal = parseEventLine (R"({"protocol":1,"type":"failure","code":"x","message":"y"})");
            const auto* defaults = as<FailureEvent> (minimal);
            expect (defaults != nullptr && ! defaults->retryable && defaults->suggestedAction.isEmpty());
        }

        beginTest ("result, with a non-ASCII path escaped on the wire");
        {
            const auto event = parseEventLine (
                R"({"protocol":1,"type":"result","cacheDir":"C:\\M\u00fasic\\\u66f2 \ud83c\udfb5","summary":{"bpm":120.5}})");
            const auto* result = as<ResultEvent> (event);
            expect (result != nullptr);
            expect (terminalKind (event) == TerminalKind::result);

            if (result != nullptr)
            {
                expectEquals (
                    result->cacheDir,
                    juce::String (juce::CharPointer_UTF8 ("C:\\M\xc3\xbasic\\\xe6\x9b\xb2 \xf0\x9f\x8e\xb5")));
                expectWithinAbsoluteError ((double) result->summary["bpm"], 120.5, 1.0e-9);
            }
        }
    }

    void toleratesUnknownAndExtraContent()
    {
        beginTest ("unknown event type is reported as unknown, not an error");
        {
            const auto event = parseEventLine (R"({"protocol":1,"type":"diagnostic","text":"hi"})");
            const auto* unknown = as<UnknownEvent> (event);
            expect (unknown != nullptr);
            expect (! isTerminal (event));

            if (unknown != nullptr)
                expectEquals (unknown->type, juce::String ("diagnostic"));
        }

        beginTest ("additive fields and surrounding whitespace are ignored");
        {
            const auto event = parseEventLine ("  "
                                               "{\"protocol\":1,\"type\":\"accepted\",\"jobId\":\"j\",\"device\":"
                                               "\"cpu\",\"future\":[1,{\"a\":2}]}\r\n");
            expect (as<AcceptedEvent> (event) != nullptr);
        }
    }

    void rejectsMalformedLines()
    {
        beginTest ("malformed lines become InvalidLine");

        const char* lines[] {
            "",
            "   ",
            "not json",
            "{\"protocol\":1,\"type\":",
            "[1,2,3]",
            "[{\"protocol\":1,\"type\":\"accepted\"}]",
            "\"text\"",
            "42",
            "null",
            R"({"type":"accepted","jobId":"j","device":"cpu"})",
            R"({"protocol":"1","type":"accepted","jobId":"j","device":"cpu"})",
            R"({"protocol":1.5,"type":"accepted","jobId":"j","device":"cpu"})",
            R"({"protocol":true,"type":"accepted","jobId":"j","device":"cpu"})",
            R"({"protocol":1})",
            R"({"protocol":1,"type":5})",
            R"({"protocol":1,"type":""})",
            R"({"protocol":1,"type":"accepted","device":"cpu"})",
            R"({"protocol":1,"type":"accepted","jobId":7,"device":"cpu"})",
            R"({"protocol":1,"type":"stage","stage":"mixing","index":1,"count":6})",
            R"({"protocol":1,"type":"stage","stage":"decoding","count":6})",
            R"({"protocol":1,"type":"progress","stage":"decoding"})",
            R"({"protocol":1,"type":"progress","stage":"decoding","fraction":"half"})",
            R"({"protocol":1,"type":"progress","stage":"decoding","fraction":0.5,"etaSeconds":"soon"})",
            R"({"protocol":1,"type":"failure","code":"x"})",
            R"({"protocol":1,"type":"failure","code":"x","message":"y","retryable":"yes"})",
            R"({"protocol":1,"type":"result","summary":{}})",
            R"({"protocol":1,"type":"result","cacheDir":"C:\\c","summary":[1]})"
        };

        for (const auto* line : lines)
        {
            const auto event = parseEventLine (line);
            expect (as<InvalidLine> (event) != nullptr, juce::String ("should be invalid: ") + line);

            if (const auto* invalid = as<InvalidLine> (event))
                expect (invalid->reason.isNotEmpty());

            expect (! isTerminal (event));
        }
    }

    void rejectsUnsupportedVersions()
    {
        beginTest ("a protocol other than 1 is unsupported, whatever the type");

        for (const auto* line : { R"({"protocol":2,"type":"accepted","jobId":"j","device":"cpu"})",
                                  R"({"protocol":0,"type":"progress"})",
                                  R"({"protocol":99,"type":"mystery"})" })
        {
            const auto event = parseEventLine (line);
            expect (as<UnsupportedProtocolLine> (event) != nullptr, juce::String (line));
            expect (! isTerminal (event));
        }
    }

    void buildsRequests()
    {
        beginTest ("prepare request without artwork");
        {
            PrepareRequest request;
            request.jobId = "job-1";
            request.sourceFile = juce::File ("C:\\Music\\song.wav");
            request.sourceSizeBytes = 5000000000LL;
            request.sourceSha256 = juce::String::repeatedString ("ab", 32);
            request.jobDir = juce::File ("C:\\snds\\jobs\\job-1");
            request.cacheDir = juce::File ("C:\\snds\\cache\\key");

            const auto line = buildPrepareRequest (request);
            expect (! line.containsAnyOf ("\r\n"));

            const auto parsed = juce::JSON::parse (line);
            expect (parsed.getDynamicObject() != nullptr);
            expectEquals ((int) parsed["protocol"], 1);
            expectEquals (parsed["type"].toString(), juce::String ("prepare"));
            expectEquals (parsed["jobId"].toString(), juce::String ("job-1"));
            expectEquals (parsed["source"]["path"].toString(), juce::String ("C:\\Music\\song.wav"));
            expect ((juce::int64) parsed["source"]["sizeBytes"] == 5000000000LL);
            expectEquals (parsed["source"]["sha256"].toString(), request.sourceSha256);
            expectEquals (parsed["jobDir"].toString(), juce::String ("C:\\snds\\jobs\\job-1"));
            expectEquals (parsed["cacheDir"].toString(), juce::String ("C:\\snds\\cache\\key"));
            expectEquals (parsed["compute"]["preference"].toString(), juce::String ("auto"));
            expect (! parsed.getDynamicObject()->hasProperty ("artwork"));
        }

        beginTest ("prepare request with CPU preference, artwork, and non-ASCII paths");
        {
            const juce::String sourceName (
                juce::CharPointer_UTF8 ("C:\\M\xc3\xbasic\\\xe6\x9b\xb2 \xf0\x9f\x8e\xb5.mp3"));

            PrepareRequest request;
            request.jobId = "job-2";
            request.sourceFile = juce::File (sourceName);
            request.sourceSha256 = juce::String::repeatedString ("0f", 32);
            request.jobDir = juce::File ("C:\\snds\\jobs\\job-2");
            request.cacheDir = juce::File ("C:\\snds\\cache\\key2");
            request.forceCpu = true;
            request.artwork = PrepareRequest::Artwork { juce::File ("C:\\snds\\jobs\\job-2\\art.png"), "embedded" };

            const auto line = buildPrepareRequest (request);
            const auto parsed = juce::JSON::parse (line);

            expectEquals (parsed["source"]["path"].toString(), sourceName);
            expectEquals (parsed["compute"]["preference"].toString(), juce::String ("cpu"));
            expectEquals (parsed["artwork"]["path"].toString(), juce::String ("C:\\snds\\jobs\\job-2\\art.png"));
            expectEquals (parsed["artwork"]["origin"].toString(), juce::String ("embedded"));

            // The worker reads the line as UTF-8 either way; ASCII keeps pipes and logs safe.
            bool ascii = true;

            for (auto character : line)
                ascii = ascii && character < 128;

            expect (ascii, "request should be pure ASCII");
        }

        beginTest ("cancel line");
        {
            const auto parsed = juce::JSON::parse (buildCancelLine());
            expectEquals ((int) parsed["protocol"], 1);
            expectEquals (parsed["type"].toString(), juce::String ("cancel"));
        }
    }

    void mapsExitCodes()
    {
        beginTest ("exit codes");
        expect (classifyExitCode (0) == ExitMeaning::ok);
        expect (classifyExitCode (10) == ExitMeaning::invalidRequest);
        expect (classifyExitCode (11) == ExitMeaning::unsupportedProtocol);
        expect (classifyExitCode (12) == ExitMeaning::cancelled);
        expect (classifyExitCode (13) == ExitMeaning::jobFailed);
        expect (classifyExitCode (14) == ExitMeaning::internalError);
        expect (classifyExitCode (99) == ExitMeaning::killedByApp);

        for (const auto code : { 1, 3, 15, 98, 100, -1, (int) 0xC0000005, (int) 0xC0000409 })
            expect (classifyExitCode (code) == ExitMeaning::crashed, juce::String (code));

        beginTest ("job outcome combines exit code and terminal event");
        expect (resolveJobOutcome (0, TerminalKind::result) == JobOutcome::succeeded);
        expect (resolveJobOutcome (12, TerminalKind::cancelled) == JobOutcome::cancelled);

        for (const auto code : { 10, 11, 13, 14 })
            expect (resolveJobOutcome (code, TerminalKind::failure) == JobOutcome::failed);

        expect (resolveJobOutcome (99, TerminalKind::none) == JobOutcome::killed);
        expect (resolveJobOutcome (99, TerminalKind::result) == JobOutcome::killed);
        expect (resolveJobOutcome ((int) 0xC0000005, TerminalKind::none) == JobOutcome::crashed);
        expect (resolveJobOutcome ((int) 0xC0000005, TerminalKind::result) == JobOutcome::crashed);

        expect (resolveJobOutcome (0, TerminalKind::none) == JobOutcome::protocolViolation);
        expect (resolveJobOutcome (13, TerminalKind::none) == JobOutcome::protocolViolation);
        expect (resolveJobOutcome (0, TerminalKind::failure) == JobOutcome::protocolViolation);
        expect (resolveJobOutcome (13, TerminalKind::result) == JobOutcome::protocolViolation);
        expect (resolveJobOutcome (0, TerminalKind::cancelled) == JobOutcome::protocolViolation);
        expect (resolveJobOutcome (12, TerminalKind::failure) == JobOutcome::protocolViolation);

        expect (describe (JobOutcome::crashed).isNotEmpty());
    }
};

const WorkerProtocolTests workerProtocolTests;
} // namespace
} // namespace snds::worker
