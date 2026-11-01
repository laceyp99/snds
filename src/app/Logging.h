#pragma once

#include <juce_core/juce_core.h>

#include <mutex>
#include <utility>
#include <vector>

namespace snds
{
/** Replaces the user's profile and local app data paths in text with %USERPROFILE% and
    %LOCALAPPDATA%: case-insensitive, either separator (including the doubled backslashes of
    JSON), whole path components only, longest path first. Same rule as the worker
    (worker/src/snds_worker/logs.py).
*/
class PathRedactor
{
public:
    /** Pairs of (variable name, path). Empty paths are ignored. */
    explicit PathRedactor (const std::vector<std::pair<juce::String, juce::String>>& variables);

    /** Uses this process's LOCALAPPDATA and USERPROFILE. */
    static PathRedactor fromEnvironment();

    juce::String redact (const juce::String& text) const;

private:
    struct Rule
    {
        std::u32string token;
        std::vector<std::u32string> components;
        size_t length = 0;
    };

    std::vector<Rule> rules;
};

/** Thread-safe line logger for the app.

    Writes <file> and rotates it to <file>.1 ... <file>.<backupCount> before a line would take
    it past maxBytes. Every line is redacted, and each one reaches the OS as a single write, so
    a crash loses nothing already logged. It takes a lock and does file I/O: never call it from
    the audio callback.
*/
class RotatingFileLogger final : public juce::Logger
{
public:
    enum class Level
    {
        info,
        warning,
        error
    };

    static constexpr juce::int64 defaultMaxBytes = 1024 * 1024;
    static constexpr int defaultBackupCount = 5;

    explicit RotatingFileLogger (juce::File file,
                                 PathRedactor redactor = PathRedactor::fromEnvironment(),
                                 juce::int64 maxBytes = defaultMaxBytes,
                                 int backupCount = defaultBackupCount);
    ~RotatingFileLogger() override;

    /** False if the log file could not be opened (for example, another process holds it). */
    bool isOpen() const;

    const juce::File& getFile() const noexcept { return file; }

    void write (Level level, const juce::String& message);

    /** juce::Logger messages are logged at info level. */
    void logMessage (const juce::String& message) override { write (Level::info, message); }

private:
    void open();
    void rotate();

    const juce::File file;
    const PathRedactor redactor;
    const juce::int64 maxBytes;
    const int backupCount;

    mutable std::mutex mutex;
    std::unique_ptr<juce::FileOutputStream> stream;
    juce::int64 bytesWritten = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RotatingFileLogger)
};

/** Log through the current juce::Logger: the RotatingFileLogger once the app has installed it,
    otherwise JUCE's debug output. */
void logInfo (const juce::String& message);
void logWarning (const juce::String& message);
void logError (const juce::String& message);
} // namespace snds
