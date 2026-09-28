#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <mutex>

/**
    SNDS file logging.

    Threading contract:
    - LogService, RotatingFileLog and JuceLoggerBridge are thread-safe (one mutex per log file).
    - They format strings, allocate, take a lock and perform file I/O, so they must NEVER be called
      from the audio callback or any other real-time thread. Real-time code publishes lock-free
      state that a non-real-time thread may log later.
    - As a guard, a thread marked with ScopedRealtimeThread has its log writes refused (counted and
      reported by the next ordinary write) instead of blocking.
*/
namespace snds::logging
{

enum class LogCategory
{
    app,    ///< logs/app.log: application lifecycle, UI, audio device, renderer, cache.
    worker, ///< logs/worker.log: worker process launches, protocol traffic, worker stderr.
};

enum class LogLevel
{
    debug,
    info,
    warning,
    error,
};

const char* toString (LogCategory category) noexcept;
const char* toString (LogLevel level) noexcept;

//==============================================================================
/** Marks the current thread as real-time for the lifetime of the scope. */
class ScopedRealtimeThread
{
public:
    ScopedRealtimeThread() noexcept;
    ~ScopedRealtimeThread() noexcept;

    static bool isCurrentThreadRealtime() noexcept;

private:
    bool previous;

    JUCE_DECLARE_NON_COPYABLE (ScopedRealtimeThread)
    JUCE_DECLARE_NON_MOVEABLE (ScopedRealtimeThread)
};

//==============================================================================
/**
    Replaces personal path fragments before text reaches a log file.

    - The user profile directory (e.g. C:\Users\name) becomes %USERPROFILE%. Backslash, forward slash
      and JSON-escaped (double backslash) spellings are matched case-insensitively.
    - The account name becomes %USERNAME% where it forms a whole path segment elsewhere
      (e.g. D:\Music\name\song.mp3). Ordinary prose containing the name is left alone.
*/
class PathRedactor
{
public:
    static constexpr const char* profileToken = "%USERPROFILE%";
    static constexpr const char* userNameToken = "%USERNAME%";

    /** A redactor that changes nothing. */
    PathRedactor() = default;

    PathRedactor (const juce::String& userProfilePath, const juce::String& userName);

    /** Uses %USERPROFILE% and the logon name of the current user. */
    static PathRedactor forCurrentUser();

    juce::String redact (const juce::String& text) const;

private:
    juce::StringArray profileSpellings;
    juce::String userName;
};

//==============================================================================
/**
    Appends UTF-8 text to a size-limited file, keeping a fixed number of archives.

    With active file "app.log" and maxArchivedFiles = 5, rotation shifts app.1.log -> app.2.log ...,
    drops app.5.log, and renames app.log -> app.1.log. An entry is never split across files; an
    entry larger than maxFileBytes is written alone into a fresh file.
*/
class RotatingFileLog
{
public:
    struct Options
    {
        juce::int64 maxFileBytes = 1024 * 1024;
        int maxArchivedFiles = 5;
    };

    RotatingFileLog (juce::File activeFile, Options options);
    ~RotatingFileLog();

    /** Appends the text as-is. Returns false if the file could not be opened or written. */
    bool append (const juce::String& text);

    const juce::File& getActiveFile() const noexcept { return activeFile; }

    /** Archive file for index 1 (newest) to maxArchivedFiles (oldest). */
    juce::File getArchiveFile (int index) const;

    const Options& getOptions() const noexcept { return options; }

private:
    bool openLocked();
    void rotateLocked();

    const juce::File activeFile;
    const Options options;
    std::mutex mutex;
    std::unique_ptr<juce::FileOutputStream> stream;
    juce::int64 currentBytes = 0;

    JUCE_DECLARE_NON_COPYABLE (RotatingFileLog)
};

//==============================================================================
/** Owns the app and worker logs below one logs directory. */
class LogService
{
public:
    LogService (const juce::File& logsDirectory, PathRedactor redactor, RotatingFileLog::Options options = {});

    /** Formats, redacts and appends one entry. Never call from a real-time thread (see file comment). */
    bool write (LogCategory category, LogLevel level, const juce::String& message);

    bool info (LogCategory category, const juce::String& message) { return write (category, LogLevel::info, message); }
    bool warning (LogCategory category, const juce::String& message)
    {
        return write (category, LogLevel::warning, message);
    }
    bool error (LogCategory category, const juce::String& message)
    {
        return write (category, LogLevel::error, message);
    }

    juce::File getLogFile (LogCategory category) const;
    const PathRedactor& getRedactor() const noexcept { return redactor; }

    /** Number of writes refused because they came from a real-time thread. */
    int getRefusedRealtimeWriteCount() const noexcept { return refusedRealtimeWrites.load(); }

    /** "<ISO-8601 time> <LEVEL> message"; continuation lines are indented. */
    static juce::String formatEntry (juce::Time time, LogLevel level, const juce::String& message);

private:
    RotatingFileLog& logFor (LogCategory category) noexcept;

    const PathRedactor redactor;
    RotatingFileLog appLog;
    RotatingFileLog workerLog;
    std::atomic<int> refusedRealtimeWrites { 0 };
    std::atomic<int> reportedRealtimeWrites { 0 };

    JUCE_DECLARE_NON_COPYABLE (LogService)
};

//==============================================================================
/**
    Routes juce::Logger::writeToLog() into the app log. Install it with juce::Logger::setCurrentLogger()
    and uninstall it before destroying the bridge or the LogService.
*/
class JuceLoggerBridge final : public juce::Logger
{
public:
    explicit JuceLoggerBridge (LogService& service);

    void logMessage (const juce::String& message) override;

private:
    LogService& service;
};

} // namespace snds::logging
