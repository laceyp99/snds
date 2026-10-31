#include "app/Logging.h"

#include <juce_events/juce_events.h>

#include <algorithm>

namespace snds
{
namespace
{
static_assert (sizeof (char32_t) == sizeof (juce::juce_wchar));

std::u32string toUtf32 (const juce::String& text)
{
    std::u32string result;
    result.reserve (static_cast<size_t> (text.length()));

    for (auto p = text.getCharPointer(); ! p.isEmpty();)
        result.push_back (static_cast<char32_t> (p.getAndAdvance()));

    return result;
}

juce::String fromUtf32 (const std::u32string& text)
{
    return juce::String (
        juce::CharPointer_UTF32 (reinterpret_cast<const juce::CharPointer_UTF32::CharType*> (text.c_str())));
}

bool isSeparator (char32_t c)
{
    return c == U'\\' || c == U'/';
}

char32_t toLower (char32_t c)
{
    return static_cast<char32_t> (juce::CharacterFunctions::toLowerCase (static_cast<juce::juce_wchar> (c)));
}

bool continuesPathComponent (char32_t c)
{
    return c == U'_' || c == U'-' || juce::CharacterFunctions::isLetterOrDigit (static_cast<juce::juce_wchar> (c));
}

std::vector<std::u32string> splitPath (const std::u32string& path)
{
    std::vector<std::u32string> components;
    std::u32string current;

    for (auto c : path)
    {
        if (! isSeparator (c))
        {
            current.push_back (c);
        }
        else if (! current.empty())
        {
            components.push_back (current);
            current.clear();
        }
    }

    if (! current.empty())
        components.push_back (current);

    return components;
}

// Length of the path made of `components` starting at text[start], or 0 if there is none.
size_t matchPath (const std::u32string& text, size_t start, const std::vector<std::u32string>& components)
{
    auto position = start;

    for (size_t index = 0; index < components.size(); ++index)
    {
        if (index > 0)
        {
            if (position >= text.size() || ! isSeparator (text[position]))
                return 0;

            while (position < text.size() && isSeparator (text[position]))
                ++position;
        }

        const auto& component = components[index];

        if (text.size() - position < component.size())
            return 0;

        for (size_t offset = 0; offset < component.size(); ++offset)
            if (toLower (text[position + offset]) != toLower (component[offset]))
                return 0;

        position += component.size();
    }

    if (position < text.size() && continuesPathComponent (text[position]))
        return 0;

    return position - start;
}

const char* levelName (RotatingFileLogger::Level level)
{
    switch (level)
    {
        case RotatingFileLogger::Level::info:
            return "INFO";
        case RotatingFileLogger::Level::warning:
            return "WARNING";
        case RotatingFileLogger::Level::error:
            return "ERROR";
    }

    return "INFO";
}

juce::String currentThreadLabel()
{
    if (juce::MessageManager::existsAndIsCurrentThread())
        return "message";

    if (auto* thread = juce::Thread::getCurrentThread())
        return thread->getThreadName();

    return "thread "
         + juce::String::toHexString (reinterpret_cast<juce::pointer_sized_int> (juce::Thread::getCurrentThreadId()));
}

juce::String timestamp()
{
    const auto now = juce::Time::getCurrentTime();
    return now.formatted ("%Y-%m-%d %H:%M:%S.") + juce::String (now.getMilliseconds()).paddedLeft ('0', 3);
}

void logAt (RotatingFileLogger::Level level, const juce::String& message)
{
    if (auto* logger = dynamic_cast<RotatingFileLogger*> (juce::Logger::getCurrentLogger()))
        logger->write (level, message);
    else
        juce::Logger::writeToLog (juce::String (levelName (level)) + " " + message);
}
} // namespace

//==============================================================================
PathRedactor::PathRedactor (const std::vector<std::pair<juce::String, juce::String>>& variables)
{
    for (const auto& [name, path] : variables)
    {
        const auto trimmed = toUtf32 (path.trim().trimCharactersAtEnd ("\\/"));
        auto components = splitPath (trimmed);

        if (! components.empty())
            rules.push_back ({ toUtf32 ("%" + name + "%"), std::move (components), trimmed.size() });
    }

    // Longest first, so %LOCALAPPDATA% wins over the %USERPROFILE% that usually contains it.
    std::stable_sort (rules.begin(), rules.end(), [] (const Rule& a, const Rule& b) { return a.length > b.length; });
}

PathRedactor PathRedactor::fromEnvironment()
{
    return PathRedactor ({ { "LOCALAPPDATA", juce::SystemStats::getEnvironmentVariable ("LOCALAPPDATA", {}) },
                           { "USERPROFILE", juce::SystemStats::getEnvironmentVariable ("USERPROFILE", {}) } });
}

juce::String PathRedactor::redact (const juce::String& input) const
{
    if (rules.empty() || input.isEmpty())
        return input;

    auto text = toUtf32 (input);

    for (const auto& rule : rules)
    {
        std::u32string result;
        result.reserve (text.size());

        for (size_t position = 0; position < text.size();)
        {
            if (const auto length = matchPath (text, position, rule.components); length > 0)
            {
                result += rule.token;
                position += length;
            }
            else
            {
                result.push_back (text[position++]);
            }
        }

        text = std::move (result);
    }

    return fromUtf32 (text);
}

//==============================================================================
RotatingFileLogger::RotatingFileLogger (juce::File logFile,
                                        PathRedactor pathRedactor,
                                        juce::int64 maximumBytes,
                                        int backups)
    : file (std::move (logFile)),
      redactor (std::move (pathRedactor)),
      maxBytes (maximumBytes),
      backupCount (juce::jmax (0, backups))
{
    const std::scoped_lock lock (mutex);
    open();
}

RotatingFileLogger::~RotatingFileLogger() = default;

bool RotatingFileLogger::isOpen() const
{
    const std::scoped_lock lock (mutex);
    return stream != nullptr;
}

void RotatingFileLogger::write (Level level, const juce::String& message)
{
    const auto line = timestamp() + " [" + currentThreadLabel() + "] " + levelName (level) + " "
                    + redactor.redact (message) + juce::newLine;
    const auto size = static_cast<juce::int64> (line.getNumBytesAsUTF8());

    const std::scoped_lock lock (mutex);

    if (stream == nullptr)
        open();

    if (stream != nullptr && bytesWritten > 0 && bytesWritten + size > maxBytes)
        rotate();

    if (stream != nullptr && stream->write (line.toRawUTF8(), static_cast<size_t> (size)))
        bytesWritten += size;
}

void RotatingFileLogger::open()
{
    stream.reset();

    if (file.getParentDirectory().createDirectory().failed())
        return;

    // No buffer: every line goes straight to the OS, so nothing logged is lost if the app
    // crashes, without paying for a disk flush per line.
    auto candidate = std::make_unique<juce::FileOutputStream> (file, 0);

    if (candidate->openedOk())
    {
        bytesWritten = candidate->getPosition();
        stream = std::move (candidate);
    }
}

void RotatingFileLogger::rotate()
{
    stream.reset();

    const auto backup = [this] (int index)
    { return file.getSiblingFile (file.getFileName() + "." + juce::String (index)); };
    auto rotated = false;

    if (backupCount == 0)
    {
        rotated = file.deleteFile();
    }
    else
    {
        backup (backupCount).deleteFile();

        for (auto index = backupCount - 1; index >= 1; --index)
            if (backup (index).existsAsFile())
                backup (index).moveFileTo (backup (index + 1));

        rotated = file.moveFileTo (backup (1));
    }

    open();

    // If another process holds the file, keep appending and retry after another maxBytes.
    if (! rotated)
        bytesWritten = 0;
}

//==============================================================================
void logInfo (const juce::String& message)
{
    logAt (RotatingFileLogger::Level::info, message);
}

void logWarning (const juce::String& message)
{
    logAt (RotatingFileLogger::Level::warning, message);
}

void logError (const juce::String& message)
{
    logAt (RotatingFileLogger::Level::error, message);
}
} // namespace snds
