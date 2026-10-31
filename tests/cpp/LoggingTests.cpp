#include <juce_core/juce_core.h>

#include "TestHelpers.h"
#include "app/Logging.h"

#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace snds
{
namespace
{
const PathRedactor testRedactor ({ { "USERPROFILE", "C:\\Users\\Pat" },
                                   { "LOCALAPPDATA", "C:\\Users\\Pat\\AppData\\Local" } });

juce::StringArray readLines (const juce::File& file)
{
    juce::StringArray lines;
    file.readLines (lines);
    lines.removeEmptyStrings();
    return lines;
}

bool looksLikeLogLine (const juce::String& line)
{
    // "2026-10-03 18:41:39.135 [thread] LEVEL message"
    return line.length() > 24 && line[4] == '-' && line[7] == '-' && line[10] == ' ' && line[13] == ':'
        && line[16] == ':' && line[19] == '.' && line[23] == ' ' && line[24] == '[';
}

class PathRedactorTests final : public juce::UnitTest
{
public:
    PathRedactorTests()
        : juce::UnitTest ("PathRedactor", "snds")
    {
    }

    void runTest() override
    {
        // Same cases as worker/tests/test_logs.py::test_redact.
        beginTest ("Redacts like the worker");
        {
            const std::pair<const char*, const char*> cases[] = {
                { "C:\\Users\\Pat\\Music\\song.mp3", "%USERPROFILE%\\Music\\song.mp3" },
                { "c:\\users\\PAT\\music", "%USERPROFILE%\\music" },
                { "C:/Users/Pat/Music", "%USERPROFILE%/Music" },
                { "C:\\Users\\Pat\\AppData\\Local\\snds\\logs", "%LOCALAPPDATA%\\snds\\logs" },
                { "{\"path\": \"C:\\\\Users\\\\Pat\\\\AppData\\\\Local\\\\snds\"}",
                  "{\"path\": \"%LOCALAPPDATA%\\\\snds\"}" },
                { "home is C:\\Users\\Pat.", "home is %USERPROFILE%." },
                { "C:\\Users\\Patty\\Music", "C:\\Users\\Patty\\Music" },
                { "D:\\Music\\song.mp3", "D:\\Music\\song.mp3" },
            };

            for (const auto& [input, expected] : cases)
                expectEquals (testRedactor.redact (input), juce::String (expected));
        }

        beginTest ("Unset variables change nothing");
        {
            const PathRedactor empty ({ { "USERPROFILE", "" }, { "LOCALAPPDATA", "  " } });
            expectEquals (empty.redact ("C:\\Users\\Pat\\x"), juce::String ("C:\\Users\\Pat\\x"));
        }

        beginTest ("Trailing separators and non-ASCII text are handled");
        {
            const PathRedactor redactor ({ { "USERPROFILE", "C:\\Users\\Pat\\" } });
            // String literals are split after each hex escape so the next character isn't read as a hex digit.
            const auto input = juce::String (juce::CharPointer_UTF8 ("Caf\xc3\xa9 C:\\Users\\Pat\\Mus\xc3\xaf"
                                                                     "c"));
            const auto expected = juce::String (juce::CharPointer_UTF8 ("Caf\xc3\xa9 %USERPROFILE%\\Mus\xc3\xaf"
                                                                        "c"));
            expectEquals (redactor.redact (input), expected);
        }
    }
};

class RotatingFileLoggerTests final : public juce::UnitTest
{
public:
    RotatingFileLoggerTests()
        : juce::UnitTest ("RotatingFileLogger", "snds")
    {
    }

    void runTest() override
    {
        beginTest ("Creates the folder and writes redacted lines with levels");
        {
            const test::ScopedTempDirectory temp;
            const auto file = temp.child ("logs\\app.log");

            {
                RotatingFileLogger logger (file, testRedactor);
                expect (logger.isOpen());
                logger.write (RotatingFileLogger::Level::info, "cache at C:\\Users\\Pat\\AppData\\Local\\snds\\cache");
                logger.write (RotatingFileLogger::Level::warning, "careful");
                logger.write (RotatingFileLogger::Level::error, "broken");
            }

            const auto lines = readLines (file);
            expectEquals (lines.size(), 3);
            expect (! lines.joinIntoString ("\n").contains ("C:\\Users\\Pat"));
            expect (lines[0].endsWith ("] INFO cache at %LOCALAPPDATA%\\snds\\cache"), lines[0]);
            expect (lines[1].endsWith ("] WARNING careful"), lines[1]);
            expect (lines[2].endsWith ("] ERROR broken"), lines[2]);

            for (const auto& line : lines)
                expect (looksLikeLogLine (line), line);

            beginTest ("Appends to an existing log");
            {
                RotatingFileLogger logger (file, testRedactor);
                logger.write (RotatingFileLogger::Level::info, "again");
            }

            expectEquals (readLines (file).size(), 4);
        }

        beginTest ("Rotates into numbered backups and keeps only backupCount of them");
        {
            const test::ScopedTempDirectory temp;
            const auto file = temp.child ("app.log");

            {
                RotatingFileLogger logger (file, testRedactor, 2000, 5);

                for (int i = 0; i < 400; ++i)
                    logger.write (RotatingFileLogger::Level::info, "line " + juce::String (i).paddedLeft ('0', 3));
            }

            for (int i = 1; i <= 5; ++i)
            {
                const auto backup = temp.child ("app.log." + juce::String (i));
                expect (backup.existsAsFile(), backup.getFileName());
                expect (backup.getSize() <= 2000);
            }

            expect (! temp.child ("app.log.6").exists());
            expect (file.getSize() <= 2000);
            expect (readLines (file).strings.getLast().endsWith ("line 399"));
            expect (readLines (temp.child ("app.log.1")).strings.getLast().getTrailingIntValue()
                    < readLines (file)[0].getTrailingIntValue());
        }

        beginTest ("Concurrent writers never interleave or lose lines");
        {
            const test::ScopedTempDirectory temp;
            const auto file = temp.child ("app.log");
            constexpr int writers = 8;
            constexpr int linesPerWriter = 250;

            {
                RotatingFileLogger logger (file, testRedactor);
                std::vector<std::thread> threads;

                for (int writer = 0; writer < writers; ++writer)
                    threads.emplace_back (
                        [&logger, writer]
                        {
                            for (int i = 0; i < linesPerWriter; ++i)
                                logger.write (RotatingFileLogger::Level::info,
                                              "writer " + juce::String (writer) + " line " + juce::String (i));
                        });

                for (auto& thread : threads)
                    thread.join();
            }

            const auto lines = readLines (file);
            expectEquals (lines.size(), writers * linesPerWriter);

            for (int writer = 0; writer < writers; ++writer)
            {
                int count = 0;

                for (const auto& line : lines)
                    if (line.contains ("] INFO writer " + juce::String (writer) + " line "))
                        ++count;

                expectEquals (count, linesPerWriter);
            }

            for (const auto& line : lines)
                expect (looksLikeLogLine (line), line);
        }

        beginTest ("Concurrent writers survive rotation");
        {
            const test::ScopedTempDirectory temp;
            const auto file = temp.child ("app.log");

            {
                RotatingFileLogger logger (file, testRedactor, 4096, 3);
                std::vector<std::thread> threads;

                for (int writer = 0; writer < 4; ++writer)
                    threads.emplace_back (
                        [&logger]
                        {
                            for (int i = 0; i < 300; ++i)
                                logger.write (RotatingFileLogger::Level::info, "rotating " + juce::String (i));
                        });

                for (auto& thread : threads)
                    thread.join();
            }

            expectEquals (temp.get().getNumberOfChildFiles (juce::File::findFiles), 4);

            for (const auto* fileName : { "app.log", "app.log.1", "app.log.2", "app.log.3" })
                for (const auto& line : readLines (temp.child (fileName)))
                    expect (looksLikeLogLine (line), line);
        }

        beginTest ("A log file held by another writer is reported, not fatal");
        {
            const test::ScopedTempDirectory temp;
            const auto file = temp.child ("app.log");
            RotatingFileLogger first (file, testRedactor);
            RotatingFileLogger second (file, testRedactor);

            expect (first.isOpen());
            expect (! second.isOpen());
            second.write (RotatingFileLogger::Level::info, "dropped");
            first.write (RotatingFileLogger::Level::info, "kept");
        }

        beginTest ("If rotation is blocked, logging continues in the current file");
        {
            const test::ScopedTempDirectory temp;
            const auto file = temp.child ("app.log");
            file.create();

            {
                // The C runtime shares read and write access but not delete, so the rename fails.
                std::ifstream reader (std::filesystem::path (file.getFullPathName().toWideCharPointer()));
                expect (reader.is_open());

                RotatingFileLogger logger (file, testRedactor, 500, 2);

                for (int i = 0; i < 50; ++i)
                    logger.write (RotatingFileLogger::Level::info, "blocked " + juce::String (i));
            }

            expectEquals (readLines (file).size(), 50);
            expect (! temp.child ("app.log.1").exists());
        }
    }
};

PathRedactorTests pathRedactorTests;
RotatingFileLoggerTests rotatingFileLoggerTests;
} // namespace
} // namespace snds
