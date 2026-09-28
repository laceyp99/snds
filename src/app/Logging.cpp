#include "app/Logging.h"

namespace snds::logging
{

namespace
{
thread_local bool currentThreadIsRealtime = false;

bool isWordCharacter (juce::juce_wchar c) noexcept
{
    return juce::CharacterFunctions::isLetterOrDigit (c);
}

bool isPathSeparator (juce::juce_wchar c) noexcept
{
    return c == '\\' || c == '/';
}

/** Case-insensitive replacement of whole-token occurrences of needle. */
juce::String replaceBounded (const juce::String& text,
                             const juce::String& needle,
                             const juce::String& replacement,
                             bool requireSeparatorBefore)
{
    if (needle.isEmpty() || text.isEmpty())
        return text;

    juce::String result;
    auto start = 0;

    for (;;)
    {
        const auto index = text.indexOfIgnoreCase (start, needle);

        if (index < 0)
            break;

        const auto end = index + needle.length();
        const auto before = index > 0 ? text[index - 1] : juce::juce_wchar (0);
        const auto after = end < text.length() ? text[end] : juce::juce_wchar (0);

        auto bounded = ! isWordCharacter (before) && ! isWordCharacter (after);

        if (requireSeparatorBefore)
            bounded = bounded && isPathSeparator (before);

        if (bounded)
        {
            result << text.substring (start, index) << replacement;
            start = end;
        }
        else
        {
            result << text.substring (start, index + 1);
            start = index + 1;
        }
    }

    result << text.substring (start);
    return result;
}
} // namespace

const char* toString (LogCategory category) noexcept
{
    switch (category)
    {
        case LogCategory::app:
            return "app";
        case LogCategory::worker:
            return "worker";
    }

    return "unknown";
}

const char* toString (LogLevel level) noexcept
{
    switch (level)
    {
        case LogLevel::debug:
            return "DEBUG";
        case LogLevel::info:
            return "INFO";
        case LogLevel::warning:
            return "WARNING";
        case LogLevel::error:
            return "ERROR";
    }

    return "UNKNOWN";
}

//==============================================================================
ScopedRealtimeThread::ScopedRealtimeThread() noexcept
    : previous (currentThreadIsRealtime)
{
    currentThreadIsRealtime = true;
}

ScopedRealtimeThread::~ScopedRealtimeThread() noexcept
{
    currentThreadIsRealtime = previous;
}

bool ScopedRealtimeThread::isCurrentThreadRealtime() noexcept
{
    return currentThreadIsRealtime;
}

//==============================================================================
PathRedactor::PathRedactor (const juce::String& userProfilePath, const juce::String& userNameIn)
{
    auto profile = userProfilePath.trim().unquoted().replaceCharacter ('/', '\\');

    while (profile.endsWithChar ('\\'))
        profile = profile.dropLastCharacters (1);

    // Only redact something that looks like a real directory (at least "X:\a"), never a bare drive.
    if (profile.length() >= 4)
    {
        profileSpellings.addIfNotAlreadyThere (profile.replace ("\\", "\\\\")); // JSON-escaped
        profileSpellings.addIfNotAlreadyThere (profile);
        profileSpellings.addIfNotAlreadyThere (profile.replaceCharacter ('\\', '/'));
    }

    userName = userNameIn.trim();

    if (userName.length() < 2 || userName.containsAnyOf ("\\/"))
        userName.clear();
}

PathRedactor PathRedactor::forCurrentUser()
{
    const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName();
    const auto profile = juce::SystemStats::getEnvironmentVariable ("USERPROFILE", home);
    return PathRedactor (profile, juce::SystemStats::getLogonName());
}

juce::String PathRedactor::redact (const juce::String& text) const
{
    auto result = text;

    for (const auto& spelling : profileSpellings)
        result = replaceBounded (result, spelling, profileToken, false);

    if (userName.isNotEmpty())
        result = replaceBounded (result, userName, userNameToken, true);

    return result;
}

//==============================================================================
RotatingFileLog::RotatingFileLog (juce::File activeFileIn, Options optionsIn)
    : activeFile (std::move (activeFileIn)),
      options (optionsIn)
{
    jassert (options.maxFileBytes > 0);
    jassert (options.maxArchivedFiles >= 0);
}

RotatingFileLog::~RotatingFileLog() = default;

juce::File RotatingFileLog::getArchiveFile (int index) const
{
    jassert (index >= 1);
    return activeFile.getSiblingFile (activeFile.getFileNameWithoutExtension() + "." + juce::String (index)
                                      + activeFile.getFileExtension());
}

bool RotatingFileLog::append (const juce::String& text)
{
    const auto numBytes = static_cast<juce::int64> (text.getNumBytesAsUTF8());

    if (numBytes == 0)
        return true;

    const std::lock_guard<std::mutex> lock (mutex);

    if (stream == nullptr && ! openLocked())
        return false;

    if (currentBytes > 0 && currentBytes + numBytes > options.maxFileBytes)
    {
        rotateLocked();

        if (! openLocked())
            return false;
    }

    if (! stream->write (text.toRawUTF8(), static_cast<size_t> (numBytes)))
    {
        stream.reset();
        return false;
    }

    // Flush every entry so the log is useful after a crash; log volume is low by design.
    stream->flush();
    currentBytes += numBytes;
    return true;
}

bool RotatingFileLog::openLocked()
{
    stream.reset();

    const auto parent = activeFile.getParentDirectory();

    if (! parent.isDirectory() && parent.createDirectory().failed())
        return false;

    auto newStream = std::make_unique<juce::FileOutputStream> (activeFile);

    if (newStream->failedToOpen())
        return false;

    currentBytes = newStream->getPosition(); // FileOutputStream appends at the end of existing files.
    stream = std::move (newStream);
    return true;
}

void RotatingFileLog::rotateLocked()
{
    stream.reset();

    if (options.maxArchivedFiles > 0)
    {
        getArchiveFile (options.maxArchivedFiles).deleteFile();

        for (auto index = options.maxArchivedFiles - 1; index >= 1; --index)
        {
            const auto source = getArchiveFile (index);

            if (source.existsAsFile())
                source.moveFileTo (getArchiveFile (index + 1));
        }

        if (activeFile.moveFileTo (getArchiveFile (1)))
            return;
    }

    // No archives are kept, or the active file could not be renamed (e.g. held open by another
    // process): discard it so the log stays bounded.
    activeFile.deleteFile();
}

//==============================================================================
LogService::LogService (const juce::File& logsDirectory, PathRedactor redactorIn, RotatingFileLog::Options options)
    : redactor (std::move (redactorIn)),
      appLog (logsDirectory.getChildFile ("app.log"), options),
      workerLog (logsDirectory.getChildFile ("worker.log"), options)
{
}

bool LogService::write (LogCategory category, LogLevel level, const juce::String& message)
{
    if (ScopedRealtimeThread::isCurrentThreadRealtime())
    {
        // Lock-free and allocation-free: never block or allocate on a real-time thread.
        refusedRealtimeWrites.fetch_add (1, std::memory_order_relaxed);
        return false;
    }

    const auto now = juce::Time::getCurrentTime();
    const auto refused = refusedRealtimeWrites.load (std::memory_order_relaxed);
    const auto alreadyReported = reportedRealtimeWrites.exchange (refused);

    if (refused != alreadyReported)
        appLog.append (formatEntry (now,
                                    LogLevel::warning,
                                    juce::String (refused - alreadyReported)
                                        + " log write(s) were refused because they came from a real-time thread."));

    return logFor (category).append (formatEntry (now, level, redactor.redact (message)));
}

juce::File LogService::getLogFile (LogCategory category) const
{
    return category == LogCategory::worker ? workerLog.getActiveFile() : appLog.getActiveFile();
}

juce::String LogService::formatEntry (juce::Time time, LogLevel level, const juce::String& message)
{
    const auto body = message.replace ("\r\n", "\n").trimEnd().replace ("\n", "\n    ");
    return time.toISO8601 (true) + " " + juce::String (toString (level)).paddedRight (' ', 7) + " " + body + "\n";
}

RotatingFileLog& LogService::logFor (LogCategory category) noexcept
{
    return category == LogCategory::worker ? workerLog : appLog;
}

//==============================================================================
JuceLoggerBridge::JuceLoggerBridge (LogService& serviceIn)
    : service (serviceIn)
{
}

void JuceLoggerBridge::logMessage (const juce::String& message)
{
    service.info (LogCategory::app, message);
}

} // namespace snds::logging
