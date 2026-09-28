#include "TestUtilities.h"
#include "app/Logging.h"

#include <thread>
#include <vector>

namespace snds::test
{

using namespace snds::logging;

class RotatingFileLogTests final : public juce::UnitTest
{
public:
    RotatingFileLogTests()
        : juce::UnitTest ("Rotating file log", testCategory)
    {
    }

    void runTest() override
    {
        beginTest ("Rotation keeps a bounded number of bounded files");
        {
            const ScopedTempDirectory temp;
            RotatingFileLog log (temp.get().getChildFile ("logs/app.log"), { 200, 3 });

            for (auto i = 0; i < 60; ++i)
                expect (log.append ("entry " + juce::String (i).paddedLeft ('0', 3) + " "
                                    + juce::String::repeatedString ("x", 30) + "\n"));

            expect (log.getActiveFile().existsAsFile());
            expect (log.getArchiveFile (1).getFileName() == "app.1.log");

            for (auto index = 1; index <= 3; ++index)
                expect (log.getArchiveFile (index).existsAsFile(), "archive " + juce::String (index));

            expect (! log.getArchiveFile (4).exists(), "Only maxArchivedFiles archives are kept");

            for (const auto& file :
                 { log.getActiveFile(), log.getArchiveFile (1), log.getArchiveFile (2), log.getArchiveFile (3) })
                expect (file.getSize() <= 200, file.getFileName() + " is " + juce::String (file.getSize()) + " bytes");

            const auto active = log.getActiveFile().loadFileAsString();
            const auto newestArchive = log.getArchiveFile (1).loadFileAsString();
            const auto oldestArchive = log.getArchiveFile (3).loadFileAsString();

            expect (active.contains ("entry 059"), "Newest entry is in the active file");
            expect (firstEntryNumber (oldestArchive) < firstEntryNumber (newestArchive));
            expect (firstEntryNumber (newestArchive) < firstEntryNumber (active));
            expect (! (active + newestArchive + oldestArchive).contains ("entry 000"), "Oldest entries were dropped");
        }

        beginTest ("An oversized entry is written whole into a fresh file");
        {
            const ScopedTempDirectory temp;
            RotatingFileLog log (temp.get().getChildFile ("app.log"), { 64, 2 });
            const auto big = juce::String::repeatedString ("y", 150) + "\n";

            expect (log.append ("small\n"));
            expect (log.append (big));
            expect (log.getActiveFile().loadFileAsString() == big);
            expect (log.getArchiveFile (1).loadFileAsString() == "small\n");
        }

        beginTest ("Reopening continues size accounting of the existing file");
        {
            const ScopedTempDirectory temp;
            const auto file = temp.get().getChildFile ("app.log");

            {
                RotatingFileLog first (file, { 100, 2 });
                expect (first.append (juce::String::repeatedString ("a", 80) + "\n"));
            }

            RotatingFileLog second (file, { 100, 2 });
            expect (second.append (juce::String::repeatedString ("b", 40) + "\n"));
            expect (second.getArchiveFile (1).existsAsFile(),
                    "Existing 81 bytes + 41 bytes exceeds 100, so it rotated");
            expect (file.loadFileAsString().startsWith ("b"));
        }

        beginTest ("Zero archives truncates instead of renaming");
        {
            const ScopedTempDirectory temp;
            RotatingFileLog log (temp.get().getChildFile ("app.log"), { 50, 0 });

            expect (log.append (juce::String::repeatedString ("a", 40) + "\n"));
            expect (log.append (juce::String::repeatedString ("b", 40) + "\n"));
            expect (log.getActiveFile().loadFileAsString().startsWith ("b"));
            expect (temp.get().getNumberOfChildFiles (juce::File::findFiles) == 1);
        }

        beginTest ("Concurrent writers never interleave or lose entries");
        {
            const ScopedTempDirectory temp;
            RotatingFileLog log (temp.get().getChildFile ("app.log"), { 2048, 100 });
            constexpr auto numThreads = 4;
            constexpr auto entriesPerThread = 200;

            std::vector<std::thread> threads;

            for (auto t = 0; t < numThreads; ++t)
                threads.emplace_back (
                    [&log, t]
                    {
                        for (auto i = 0; i < entriesPerThread; ++i)
                            log.append ("thread " + juce::String (t) + " entry " + juce::String (i) + " end\n");
                    });

            for (auto& thread : threads)
                thread.join();

            auto totalLines = 0;
            auto malformedLines = 0;

            for (const auto& file : temp.get().findChildFiles (juce::File::findFiles, false, "*.log"))
            {
                juce::StringArray lines;
                lines.addLines (file.loadFileAsString());
                lines.removeEmptyStrings();

                for (const auto& line : lines)
                {
                    ++totalLines;

                    if (! (line.startsWith ("thread ") && line.endsWith (" end")))
                        ++malformedLines;
                }

                expect (file.getSize() <= 2048, file.getFileName());
            }

            expectEquals (totalLines, numThreads * entriesPerThread);
            expectEquals (malformedLines, 0);
        }
    }

private:
    static int firstEntryNumber (const juce::String& text)
    {
        return text.fromFirstOccurrenceOf ("entry ", false, false).substring (0, 3).getIntValue();
    }
};

//==============================================================================
class PathRedactorTests final : public juce::UnitTest
{
public:
    PathRedactorTests()
        : juce::UnitTest ("Path redaction", testCategory)
    {
    }

    void runTest() override
    {
        const PathRedactor redactor ("C:\\Users\\Jane.Doe", "Jane.Doe");

        beginTest ("Profile directory is replaced in every spelling");
        {
            expectEquals (redactor.redact ("Opened C:\\Users\\Jane.Doe\\Music\\a.mp3"),
                          juce::String ("Opened %USERPROFILE%\\Music\\a.mp3"));
            expectEquals (redactor.redact ("c:/users/jane.doe/Music/a.mp3"),
                          juce::String ("%USERPROFILE%/Music/a.mp3"));
            expectEquals (redactor.redact ("{\"path\": \"C:\\\\Users\\\\Jane.Doe\\\\x.wav\"}"),
                          juce::String ("{\"path\": \"%USERPROFILE%\\\\x.wav\"}"));
            expectEquals (redactor.redact ("C:\\Users\\Jane.Doe"), juce::String ("%USERPROFILE%"));
        }

        beginTest ("A different profile that merely shares a prefix is not treated as the profile");
        {
            expectEquals (redactor.redact ("C:\\Users\\Jane.Doe2\\x"), juce::String ("C:\\Users\\Jane.Doe2\\x"));
        }

        beginTest ("User name is replaced when it is a path segment elsewhere");
        {
            expectEquals (redactor.redact ("D:\\Music\\Jane.Doe\\song.wav"),
                          juce::String ("D:\\Music\\%USERNAME%\\song.wav"));
            expectEquals (redactor.redact ("\\\\nas\\share\\jane.doe"), juce::String ("\\\\nas\\share\\%USERNAME%"));
            expectEquals (redactor.redact ("Hello Jane.Doe"), juce::String ("Hello Jane.Doe"));
        }

        beginTest ("Default and degenerate redactors change nothing");
        {
            expectEquals (PathRedactor().redact ("C:\\Users\\Jane.Doe\\x"), juce::String ("C:\\Users\\Jane.Doe\\x"));
            expectEquals (PathRedactor ("C:\\", "a").redact ("C:\\a\\b"), juce::String ("C:\\a\\b"));
        }

        beginTest ("Current-user redactor hides the real profile");
        {
            const auto home = juce::SystemStats::getEnvironmentVariable (
                "USERPROFILE",
                juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName());
            const auto redacted = PathRedactor::forCurrentUser().redact (home + "\\AppData\\Local\\snds\\logs");
            expectEquals (redacted, juce::String ("%USERPROFILE%\\AppData\\Local\\snds\\logs"));
        }
    }
};

//==============================================================================
class LogServiceTests final : public juce::UnitTest
{
public:
    LogServiceTests()
        : juce::UnitTest ("Log service", testCategory)
    {
    }

    void runTest() override
    {
        beginTest ("App and worker categories write separate, redacted files");
        {
            const ScopedTempDirectory temp;
            LogService service (temp.get(), PathRedactor ("C:\\Users\\Jane", "Jane"));

            expect (service.info (LogCategory::app, "Opened C:\\Users\\Jane\\a.wav"));
            expect (service.warning (LogCategory::worker, "worker says hi"));

            const auto appText = service.getLogFile (LogCategory::app).loadFileAsString();
            const auto workerText = service.getLogFile (LogCategory::worker).loadFileAsString();

            expectEquals (service.getLogFile (LogCategory::app).getFileName(), juce::String ("app.log"));
            expectEquals (service.getLogFile (LogCategory::worker).getFileName(), juce::String ("worker.log"));
            expect (appText.contains ("INFO    Opened %USERPROFILE%\\a.wav"), appText);
            expect (! appText.contains ("worker says hi"));
            expect (workerText.contains ("WARNING worker says hi"), workerText);
        }

        beginTest ("Entries are timestamped and continuation lines indented");
        {
            const auto entry = LogService::formatEntry (juce::Time (2026, 8, 27, 10, 15, 30, 0, true),
                                                        LogLevel::error,
                                                        "first\r\nsecond\n");
            expect (entry.startsWith ("2026-09-27T10:15:30"), entry);
            expect (entry.endsWith (" ERROR   first\n    second\n"), entry);
        }

        beginTest ("Real-time threads are refused without blocking, then reported");
        {
            const ScopedTempDirectory temp;
            LogService service (temp.get(), {});

            {
                const ScopedRealtimeThread realtime;
                expect (ScopedRealtimeThread::isCurrentThreadRealtime());
                expect (! service.info (LogCategory::app, "from the audio callback"));
            }

            expect (! ScopedRealtimeThread::isCurrentThreadRealtime());
            expectEquals (service.getRefusedRealtimeWriteCount(), 1);

            expect (service.info (LogCategory::app, "normal"));
            const auto text = service.getLogFile (LogCategory::app).loadFileAsString();
            expect (! text.contains ("from the audio callback"));
            expect (text.contains ("1 log write(s) were refused"), text);
        }
    }
};

static RotatingFileLogTests rotatingFileLogTests;
static PathRedactorTests pathRedactorTests;
static LogServiceTests logServiceTests;

} // namespace snds::test
