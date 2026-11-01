#include "app/WorkerProcess.h"

#include <juce_events/juce_events.h>

#include "app/Logging.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace snds
{
namespace
{
using namespace worker;

constexpr size_t maxLineBytes = 4 * 1024 * 1024;
constexpr int stderrTailChars = 4000;
constexpr DWORD pipeBufferBytes = 64 * 1024;

double millisecondsNow()
{
    return juce::Time::getMillisecondCounterHiRes();
}

/** Owns a Win32 handle. */
class Handle
{
public:
    Handle() = default;
    explicit Handle (HANDLE h) noexcept
        : handle (h)
    {
    }
    Handle (Handle&& other) noexcept
        : handle (other.release())
    {
    }

    Handle& operator= (Handle&& other) noexcept
    {
        reset (other.release());
        return *this;
    }

    ~Handle() { reset(); }

    HANDLE get() const noexcept { return handle; }
    bool valid() const noexcept { return handle != nullptr && handle != INVALID_HANDLE_VALUE; }

    HANDLE release() noexcept
    {
        const auto h = handle;
        handle = nullptr;
        return h;
    }

    void reset (HANDLE replacement = nullptr) noexcept
    {
        if (valid())
            CloseHandle (handle);

        handle = replacement;
    }

private:
    HANDLE handle = nullptr;

    Handle (const Handle&) = delete;
    Handle& operator= (const Handle&) = delete;
};

struct Pipe
{
    Handle parentEnd;
    Handle childEnd;
};

/** A pipe whose child end is inheritable and whose parent end is not. */
bool makePipe (Pipe& pipe, bool parentReads)
{
    SECURITY_ATTRIBUTES attributes {};
    attributes.nLength = sizeof (attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;

    if (! CreatePipe (&readEnd, &writeEnd, &attributes, pipeBufferBytes))
        return false;

    pipe.parentEnd.reset (parentReads ? readEnd : writeEnd);
    pipe.childEnd.reset (parentReads ? writeEnd : readEnd);
    return SetHandleInformation (pipe.parentEnd.get(), HANDLE_FLAG_INHERIT, 0) != 0;
}

juce::String lastErrorText (const juce::String& what)
{
    const auto code = GetLastError();
    return what + " failed (Win32 error " + juce::String (static_cast<juce::int64> (code)) + ")";
}

std::wstring toWide (const juce::String& text)
{
    return std::wstring (text.toWideCharPointer());
}

bool writeAll (HANDLE handle, const juce::String& text)
{
    const auto utf8 = text.toStdString();
    size_t offset = 0;

    while (offset < utf8.size())
    {
        DWORD written = 0;
        const auto chunk = static_cast<DWORD> (std::min<size_t> (utf8.size() - offset, 32 * 1024));

        if (! WriteFile (handle, utf8.data() + offset, chunk, &written, nullptr) || written == 0)
            return false;

        offset += written;
    }

    return true;
}

/** The parent's environment with the extra variables applied, as a CreateProcess block. */
std::vector<wchar_t> buildEnvironmentBlock (const juce::StringPairArray& extra)
{
    std::vector<std::wstring> entries;

    if (auto* block = GetEnvironmentStringsW())
    {
        for (const wchar_t* cursor = block; *cursor != 0;)
        {
            entries.emplace_back (cursor);
            cursor += entries.back().size() + 1;
        }

        FreeEnvironmentStringsW (block);
    }

    const auto& keys = extra.getAllKeys();

    for (int index = 0; index < keys.size(); ++index)
    {
        const auto name = toWide (keys[index]);

        entries.erase (std::remove_if (entries.begin(),
                                       entries.end(),
                                       [&] (const std::wstring& entry)
                                       {
                                           return entry.size() > name.size() && entry[name.size()] == L'='
                                               && _wcsnicmp (entry.c_str(), name.c_str(), name.size()) == 0;
                                       }),
                       entries.end());
        entries.push_back (name + L"=" + toWide (extra.getValue (keys[index], {})));
    }

    std::vector<wchar_t> result;

    for (const auto& entry : entries)
    {
        result.insert (result.end(), entry.begin(), entry.end());
        result.push_back (L'\0');
    }

    result.push_back (L'\0');

    if (result.size() == 1)
        result.push_back (L'\0');

    return result;
}

bool isAudioIntermediate (const juce::File& file)
{
    static const juce::StringArray extensions { ".wav", ".mp3", ".flac", ".ogg", ".aif", ".aiff", ".m4a",
                                                ".raw", ".pcm", ".npy",  ".npz", ".pt",  ".th",   ".tmp" };
    return extensions.contains (file.getFileExtension().toLowerCase());
}

void removeEmptyFolders (const juce::File& folder)
{
    for (const auto& child : folder.findChildFiles (juce::File::findDirectories, false))
    {
        removeEmptyFolders (child);

        if (child.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0)
            child.deleteRecursively();
    }
}
} // namespace

//==============================================================================
juce::String quoteCommandLineArgument (const juce::String& argument)
{
    const auto wide = toWide (argument);

    if (! wide.empty() && wide.find_first_of (L" \t\n\v\"") == std::wstring::npos)
        return argument;

    std::wstring out = L"\"";
    size_t backslashes = 0;

    for (const auto character : wide)
    {
        if (character == L'\\')
        {
            ++backslashes;
            continue;
        }

        if (character == L'"')
            out.append (backslashes * 2 + 1, L'\\');
        else
            out.append (backslashes, L'\\');

        out += character;
        backslashes = 0;
    }

    out.append (backslashes * 2, L'\\');
    out += L'"';
    return juce::String (out.c_str());
}

int deleteIncompleteAudio (const juce::File& jobDir)
{
    if (! jobDir.isDirectory())
        return 0;

    int deleted = 0;

    for (const auto& file : jobDir.findChildFiles (juce::File::findFiles, true))
        if (isAudioIntermediate (file) && file.deleteFile())
            ++deleted;

    removeEmptyFolders (jobDir);
    return deleted;
}

//==============================================================================
struct WorkerProcess::Shared : std::enable_shared_from_this<WorkerProcess::Shared>
{
    WorkerProcessOptions options;
    Callbacks callbacks;
    Dispatcher dispatcher;

    std::atomic<bool> started { false };
    std::atomic<bool> finished { false };
    std::atomic<bool> detached { false }; // the owner is gone: deliver nothing more
    std::atomic<bool> cancelRequested { false };
    std::atomic<bool> abandon { false };        // the owner is being destroyed: kill now
    std::atomic<bool> protocolFailed { false }; // set by the stdout reader

    std::mutex mutex; // guards the fields below
    std::optional<Event> terminalEvent;
    juce::String protocolError;
    int invalidLines = 0;
    int unknownEvents = 0;
    juce::String stderrTail;

    void post (std::function<void()> work)
    {
        dispatcher (
            [self = shared_from_this(), work = std::move (work)]
            {
                if (! self->detached)
                    work();
            });
    }

    void handleStdoutLine (const juce::String& line);
    void handleStderrLine (const juce::String& line, juce::FileOutputStream* logFile);
    void readStdout (HANDLE handle);
    void readStderr (HANDLE handle);
    WorkerJobResult execute();
    void run();
};

namespace
{
template <typename Handler>
void readLines (HANDLE handle, Handler&& onLine)
{
    std::vector<char> buffer (16 * 1024);
    std::string pending;
    bool discarding = false;

    const auto emit = [&] (std::string line)
    {
        if (! line.empty() && line.back() == '\r')
            line.pop_back();

        onLine (juce::String::fromUTF8 (line.data(), static_cast<int> (line.size())));
    };

    for (;;)
    {
        DWORD count = 0;

        if (! ReadFile (handle, buffer.data(), static_cast<DWORD> (buffer.size()), &count, nullptr) || count == 0)
            break; // every writer closed (or the read was cancelled)

        for (DWORD index = 0; index < count; ++index)
        {
            const auto character = buffer[index];

            if (character == '\n')
            {
                if (discarding)
                    onLine ("<line too long>"); // reported as an invalid line by the caller
                else
                    emit (std::move (pending));

                pending.clear();
                discarding = false;
            }
            else if (! discarding)
            {
                pending.push_back (character);

                if (pending.size() > maxLineBytes)
                {
                    pending.clear();
                    discarding = true;
                }
            }
        }
    }

    if (! pending.empty())
        emit (std::move (pending)); // an unterminated tail
}
} // namespace

void WorkerProcess::Shared::handleStdoutLine (const juce::String& line)
{
    if (line.trim().isEmpty())
        return;

    auto event = parseEventLine (line);

    {
        const std::lock_guard<std::mutex> lock (mutex);

        if (terminalEvent.has_value())
        {
            logWarning ("Worker wrote output after its terminal event; ignored.");
            return;
        }
    }

    if (const auto* invalid = std::get_if<InvalidLine> (&event))
    {
        logWarning ("Worker stdout line skipped: " + invalid->reason);
        const std::lock_guard<std::mutex> lock (mutex);
        ++invalidLines;
        return;
    }

    if (const auto* unknown = std::get_if<UnknownEvent> (&event))
    {
        logInfo ("Worker event of unknown type " + unknown->type.quoted() + " ignored.");
        const std::lock_guard<std::mutex> lock (mutex);
        ++unknownEvents;
        return;
    }

    if (const auto* unsupported = std::get_if<UnsupportedProtocolLine> (&event))
    {
        const auto text = "The worker speaks protocol " + unsupported->protocol.toString() + "; this app speaks "
                        + juce::String (protocolVersion) + ". Rebuild the worker environment (scripts\\bootstrap.ps1).";
        logError (text);
        {
            const std::lock_guard<std::mutex> lock (mutex);
            protocolError = text;
        }
        protocolFailed = true;
        return;
    }

    if (isTerminal (event))
    {
        const std::lock_guard<std::mutex> lock (mutex);
        terminalEvent = event;
    }

    post (
        [this, event]
        {
            if (callbacks.onEvent)
                callbacks.onEvent (event);
        });
}

void WorkerProcess::Shared::handleStderrLine (const juce::String& line, juce::FileOutputStream* logFile)
{
    static const PathRedactor redactor = PathRedactor::fromEnvironment();
    const auto redacted = redactor.redact (line);

    logInfo ("worker: " + redacted);

    if (logFile != nullptr)
    {
        logFile->writeText (redacted + "\n", false, false, nullptr);
        logFile->flush();
    }

    const std::lock_guard<std::mutex> lock (mutex);
    stderrTail << redacted << "\n";

    if (stderrTail.length() > stderrTailChars * 2)
        stderrTail = stderrTail.substring (stderrTail.length() - stderrTailChars);
}

void WorkerProcess::Shared::readStdout (HANDLE handle)
{
    readLines (handle, [this] (const juce::String& line) { handleStdoutLine (line); });
}

void WorkerProcess::Shared::readStderr (HANDLE handle)
{
    std::unique_ptr<juce::FileOutputStream> logFile;
    const auto file = options.jobDir.getChildFile ("worker-stderr.log");

    if (options.jobDir.isDirectory())
    {
        logFile = std::make_unique<juce::FileOutputStream> (file);

        if (logFile->failedToOpen())
            logFile.reset();
    }

    readLines (handle, [&] (const juce::String& line) { handleStderrLine (line, logFile.get()); });
}

WorkerJobResult WorkerProcess::Shared::execute()
{
    WorkerJobResult result;
    const auto startedAt = millisecondsNow();

    const auto fail = [&] (const juce::String& message)
    {
        result.launchError = message;
        result.outcome = JobOutcome::crashed;
        result.seconds = (millisecondsNow() - startedAt) / 1000.0;
        logError ("Could not start the worker: " + message);
        return result;
    };

    if (options.commandLine.isEmpty())
        return fail ("no command");

    if (options.jobDir != juce::File() && ! options.jobDir.createDirectory())
        return fail ("could not create the job folder");

    // --- pipes, job, process ---------------------------------------------------------------
    Pipe input, output, errors;

    if (! makePipe (input, false) || ! makePipe (output, true) || ! makePipe (errors, true))
        return fail (lastErrorText ("CreatePipe"));

    Handle job (CreateJobObjectW (nullptr, nullptr));

    if (! job.valid())
        return fail (lastErrorText ("CreateJobObject"));

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    if (! SetInformationJobObject (job.get(), JobObjectExtendedLimitInformation, &limits, sizeof (limits)))
        return fail (lastErrorText ("SetInformationJobObject"));

    // Only the three child pipe ends are inherited, so concurrent launches never leak each
    // other's handles into their children.
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList (nullptr, 1, 0, &attributeBytes);
    std::vector<char> attributeStorage (attributeBytes);
    auto* attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST> (attributeStorage.data());

    if (! InitializeProcThreadAttributeList (attributeList, 1, 0, &attributeBytes))
        return fail (lastErrorText ("InitializeProcThreadAttributeList"));

    struct AttributeListCleanup
    {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~AttributeListCleanup() { DeleteProcThreadAttributeList (list); }
    } attributeCleanup { attributeList };

    HANDLE inherited[] { input.childEnd.get(), output.childEnd.get(), errors.childEnd.get() };

    if (! UpdateProcThreadAttribute (
            attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof (inherited), nullptr, nullptr))
        return fail (lastErrorText ("UpdateProcThreadAttribute"));

    STARTUPINFOEXW startup {};
    startup.StartupInfo.cb = sizeof (startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.childEnd.get();
    startup.StartupInfo.hStdOutput = output.childEnd.get();
    startup.StartupInfo.hStdError = errors.childEnd.get();
    startup.lpAttributeList = attributeList;

    std::wstring commandLine;

    for (const auto& argument : options.commandLine)
        commandLine += (commandLine.empty() ? L"" : L" ") + toWide (quoteCommandLineArgument (argument));

    commandLine.push_back (L'\0');

    const auto applicationName = toWide (options.commandLine[0]);
    const auto workingDirectory = toWide (options.workingDirectory.getFullPathName());
    auto environment = buildEnvironmentBlock (options.extraEnvironment);

    PROCESS_INFORMATION processInfo {};

    if (! CreateProcessW (applicationName.c_str(),
                          commandLine.data(),
                          nullptr,
                          nullptr,
                          TRUE,
                          CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT
                              | CREATE_UNICODE_ENVIRONMENT,
                          environment.data(),
                          options.workingDirectory != juce::File() ? workingDirectory.c_str() : nullptr,
                          &startup.StartupInfo,
                          &processInfo))
        return fail (lastErrorText ("CreateProcess"));

    Handle process (processInfo.hProcess);
    Handle mainThread (processInfo.hThread);

    // Into the job while still suspended, so nothing it starts can be outside the job.
    if (! AssignProcessToJobObject (job.get(), process.get()))
    {
        const auto message = lastErrorText ("AssignProcessToJobObject");
        TerminateProcess (process.get(), static_cast<UINT> (exitcode::killedByApp));
        return fail (message);
    }

    ResumeThread (mainThread.get());
    mainThread.reset();

    // The child owns its ends now; closing ours is what lets the readers see end of file.
    input.childEnd.reset();
    output.childEnd.reset();
    errors.childEnd.reset();

    result.launched = true;
    logInfo ("Worker started (pid " + juce::String (static_cast<juce::int64> (processInfo.dwProcessId)) + ").");

    std::thread stdoutReader ([this, handle = output.parentEnd.get()] { readStdout (handle); });
    std::thread stderrReader ([this, handle = errors.parentEnd.get()] { readStderr (handle); });

    if (! writeAll (input.parentEnd.get(), options.requestLine + "\n"))
        logWarning ("Could not write the request to the worker's stdin.");

    // --- wait, cancel, timeout -------------------------------------------------------------
    bool cancelSent = false;
    bool killed = false;
    double cancelSentAt = 0.0;

    const auto terminateTree = [&]
    {
        if (! killed)
        {
            TerminateJobObject (job.get(), static_cast<UINT> (exitcode::killedByApp));
            killed = true;
            result.killedByApp = true;
        }
    };

    while (WaitForSingleObject (process.get(), 20) == WAIT_TIMEOUT)
    {
        const auto now = millisecondsNow();

        if (abandon || protocolFailed)
        {
            terminateTree();
            continue;
        }

        if (options.timeoutMs > 0 && ! cancelSent && now - startedAt >= options.timeoutMs)
        {
            result.timedOut = true;
            cancelRequested = true;
            logWarning ("The worker timed out; cancelling.");
        }

        if (cancelRequested && ! cancelSent)
        {
            // The cancel line first; closing stdin cancels as well if the line is missed.
            writeAll (input.parentEnd.get(), buildCancelLine() + "\n");
            input.parentEnd.reset();
            cancelSent = true;
            cancelSentAt = now;
        }

        if (cancelSent && now - cancelSentAt >= options.cancelGraceMs)
        {
            logWarning ("The worker did not stop after the cancel request; terminating it.");
            terminateTree();
        }
    }

    DWORD rawExit = 0;
    GetExitCodeProcess (process.get(), &rawExit);
    result.exitCode = static_cast<int> (rawExit);

    // The worker is gone; a descendant that outlived it would hold the pipes open.
    if (! killed)
        TerminateJobObject (job.get(), static_cast<UINT> (exitcode::killedByApp));

    input.parentEnd.reset();

    const auto joinReader = [] (std::thread& reader, Handle& pipe)
    {
        // Normally the pipe is at end of file by now. If not, cancel the pending read.
        const auto deadline = millisecondsNow() + 2000.0;
        std::atomic<bool> done { false };
        std::thread watchdog (
            [&]
            {
                while (! done && millisecondsNow() < deadline)
                    Sleep (10);

                if (! done)
                    CancelIoEx (pipe.get(), nullptr);
            });
        reader.join();
        done = true;
        watchdog.join();
    };

    joinReader (stdoutReader, output.parentEnd);
    joinReader (stderrReader, errors.parentEnd);

    {
        const std::lock_guard<std::mutex> lock (mutex);
        result.terminalEvent = terminalEvent;
        result.protocolError = protocolError;
        result.invalidLines = invalidLines;
        result.unknownEvents = unknownEvents;
        result.stderrTail = stderrTail;
    }

    result.cancelRequested = cancelRequested;
    result.terminalKind = result.terminalEvent ? terminalKind (*result.terminalEvent) : TerminalKind::none;
    result.outcome = result.protocolError.isNotEmpty() ? JobOutcome::protocolViolation
                                                       : resolveJobOutcome (result.exitCode, result.terminalKind);
    result.seconds = (millisecondsNow() - startedAt) / 1000.0;
    return result;
}

void WorkerProcess::Shared::run()
{
    auto result = execute();

    if (result.outcome != JobOutcome::succeeded && options.jobDir != juce::File())
        result.deletedAudioFiles = deleteIncompleteAudio (options.jobDir);

    logInfo ("Worker finished: " + describe (result.outcome) + " (exit code " + juce::String (result.exitCode) + ", "
             + juce::String (result.seconds, 1) + " s).");

    finished = true;
    post (
        [this, result]
        {
            if (callbacks.onFinished)
                callbacks.onFinished (result);
        });
}

//==============================================================================
WorkerProcess::WorkerProcess (WorkerProcessOptions options, Callbacks callbacks, Dispatcher dispatcher)
    : shared (std::make_shared<Shared>())
{
    shared->options = std::move (options);
    shared->callbacks = std::move (callbacks);
    shared->dispatcher = dispatcher != nullptr ? std::move (dispatcher)
                                               : Dispatcher ([] (std::function<void()> work)
                                                             { juce::MessageManager::callAsync (std::move (work)); });
}

WorkerProcess::~WorkerProcess()
{
    shared->detached = true;
    shared->abandon = true;

    if (supervisor.joinable())
        supervisor.join();
}

void WorkerProcess::start()
{
    jassert (! supervisor.joinable());

    if (supervisor.joinable())
        return;

    shared->started = true;
    supervisor = std::thread ([state = shared.get()] { state->run(); });
}

void WorkerProcess::cancel()
{
    shared->cancelRequested = true;
}

bool WorkerProcess::isRunning() const
{
    return shared->started && ! shared->finished;
}

WorkerProcessOptions WorkerProcess::makePrepareOptions (const WorkerLocation& location,
                                                        const worker::PrepareRequest& request)
{
    WorkerProcessOptions options;
    options.commandLine = location.commandFor ("prepare");
    options.workingDirectory = location.root;
    options.requestLine = buildPrepareRequest (request);
    options.jobDir = request.jobDir;
    return options;
}
} // namespace snds
