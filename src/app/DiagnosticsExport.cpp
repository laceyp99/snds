#include "app/DiagnosticsExport.h"

#include "app/AppInfo.h"
#include "app/CacheOverview.h"
#include "app/CacheStore.h"
#include "model/SourceFingerprint.h"

#include <algorithm>

namespace snds
{
namespace
{
constexpr juce::int64 logTailBytes = 256 * 1024; // the end of each log file is what matters

juce::String stateName (audio::DeviceStatus::State state)
{
    using State = audio::DeviceStatus::State;

    switch (state)
    {
        case State::notStarted:
            return "not started";
        case State::running:
            return "running";
        case State::fallback:
            return "fallback";
        case State::lost:
            return "lost";
        case State::failed:
            return "failed";
    }

    return {};
}

/** SHA-256 of a file read in chunks, or empty if it cannot be read. */
juce::String fileSha256 (const juce::File& file)
{
    juce::FileInputStream input (file);
    Sha256 hash;

    if (! input.openedOk() || ! hash.isValid())
        return {};

    std::vector<char> buffer (1 << 20);

    while (! input.isExhausted())
    {
        const auto read = input.read (buffer.data(), static_cast<int> (buffer.size()));

        if (read <= 0)
            break;

        hash.update (buffer.data(), static_cast<size_t> (read));
    }

    return hash.finish();
}

/** Collects `## Section` blocks; a section that throws on disk trouble says so and the report goes on. */
class Report
{
public:
    explicit Report (const PathRedactor& redactorToUse)
        : redactor (redactorToUse)
    {
    }

    void line (const juce::String& text = {}) { lines.add (redactor.redact (text)); }

    void item (const juce::String& name, const juce::String& value)
    {
        line (juce::String (name + ":").paddedRight (' ', 24) + " " + value);
    }

    void section (const juce::String& title)
    {
        lines.add ({});
        lines.add ("## " + title);
    }

    juce::String text() const { return lines.joinIntoString ("\n") + "\n"; }

private:
    const PathRedactor& redactor;
    juce::StringArray lines;
};

juce::String describeSize (const juce::File& file)
{
    return file.existsAsFile() ? formatBytes (file.getSize()) : juce::String ("missing");
}

juce::Array<juce::File> logFiles (const std::optional<DataPaths>& paths)
{
    juce::Array<juce::File> files;

    if (paths)
        files = paths->logsDirectory().findChildFiles (juce::File::findFiles, false, "*.log*");

    // Newest first, so a cap on the total keeps what happened last.
    juce::Array<juce::File> sorted;
    std::vector<juce::File> byTime (files.begin(), files.end());
    std::sort (byTime.begin(),
               byTime.end(),
               [] (const juce::File& a, const juce::File& b)
               { return a.getLastModificationTime() > b.getLastModificationTime(); });

    for (const auto& file : byTime)
        sorted.add (file);

    return sorted;
}

/** The last `logTailBytes` of a file, starting at a line start. */
juce::String readTail (const juce::File& file)
{
    juce::FileInputStream input (file);

    if (! input.openedOk())
        return "(could not be read: " + input.getStatus().getErrorMessage() + ")\n";

    const auto size = input.getTotalLength();
    const auto skipped = juce::jmax<juce::int64> (0, size - logTailBytes);
    input.setPosition (skipped);
    auto text = input.readEntireStreamAsString();

    if (skipped > 0)
    {
        const auto newline = text.indexOfChar ('\n');
        text = "(earlier lines left out)\n" + (newline >= 0 ? text.substring (newline + 1) : juce::String());
    }

    return text;
}

void writeApplication (Report& report, const DiagnosticsInput& input)
{
    report.section ("Application");
    report.item ("SNDS", appinfo::version());
    report.item ("JUCE",
                 appinfo::compiledJuceVersion()
                     + (appinfo::juceMatchesPin() ? " (pinned)" : " (NOT the pinned version)"));
#if JUCE_DEBUG
    report.item ("Build", "Debug");
#else
    report.item ("Build", "Release");
#endif
    report.item ("Executable", juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName());
    report.item ("Worker root", input.worker.root.getFullPathName() + " (" + input.worker.source + ")");
}

void writeSystem (Report& report)
{
    report.section ("System");
    report.item ("OS",
                 juce::SystemStats::getOperatingSystemName()
                     + (juce::SystemStats::isOperatingSystem64Bit() ? " (64-bit)" : ""));
    report.item ("CPU",
                 juce::SystemStats::getCpuModel() + " (" + juce::String (juce::SystemStats::getNumPhysicalCpus())
                     + " cores, " + juce::String (juce::SystemStats::getNumCpus()) + " threads)");
    report.item ("RAM", juce::String (juce::SystemStats::getMemorySizeInMegabytes() / 1024.0, 1) + " GB");
}

void writeDataDirectories (Report& report, const std::optional<DataPaths>& paths)
{
    report.section ("Data directories");

    if (! paths)
    {
        report.item ("SNDS home", "unavailable (neither SNDS_HOME nor LOCALAPPDATA is set)");
        return;
    }

    const auto fromVariable =
        juce::SystemStats::getEnvironmentVariable (DataPaths::homeVariable, {}).trim().isNotEmpty();
    report.item ("SNDS home", paths->home.getFullPathName() + (fromVariable ? " (SNDS_HOME)" : " (default)"));

    const std::vector<std::pair<juce::String, juce::File>> entries { { "settings.json", paths->settingsFile() },
                                                                     { "cache", paths->cacheDirectory() },
                                                                     { "jobs", paths->jobsDirectory() },
                                                                     { "logs", paths->logsDirectory() },
                                                                     { "models", paths->modelsDirectory() } };

    for (const auto& [name, file] : entries)
        report.item (name, file.exists() ? "present" : "missing");
}

void writeModel (Report& report, const std::optional<DataPaths>& paths)
{
    report.section ("Model");

    if (! paths)
        return report.item ("Status", "unavailable");

    const auto model = paths->modelFile();
    report.item ("Path", model.getFullPathName());

    if (! model.existsAsFile())
        return report.item ("Status", "missing (run scripts\\bootstrap.ps1)");

    report.item ("Size", juce::String (model.getSize()) + " bytes");
    report.item ("SHA-256",
                 [&]
                 {
                     const auto hash = fileSha256 (model);
                     return hash.isNotEmpty() ? hash : juce::String ("could not be read");
                 }());
}

void writeWorker (Report& report, const DiagnosticsInput& input)
{
    report.section ("Worker environment");
    report.item ("Python", input.worker.pythonExecutable().getFullPathName());
    report.item ("State", Readiness::stateName (input.readiness.state));
    report.item ("Headline", input.readiness.headline);

    if (input.readiness.guidance.isNotEmpty())
        report.item ("Guidance", input.readiness.guidance);

    // The readiness output: worker and protocol versions, torch, device, model check.
    for (const auto& detail : juce::StringArray::fromLines (input.readiness.details))
        report.line (detail);
}

void writeRenderer (Report& report, const DiagnosticsInput& input)
{
    report.section ("Renderer");
    report.line (input.rendererStatus.isNotEmpty() ? input.rendererStatus : juce::String ("not started"));
}

void writeDevice (Report& report, const DiagnosticsInput& input)
{
    report.section ("Audio device");

    if (! input.device)
        return report.line ("The app is running without audio.");

    const auto& device = *input.device;
    report.item ("State", stateName (device.state));
    report.item ("Type", device.typeName);
    report.item ("Device", device.deviceName);
    report.item ("Preferred", device.preferredDeviceName);
    report.item ("Sample rate",
                 device.sampleRate > 0.0 ? juce::String (juce::roundToInt (device.sampleRate)) + " Hz"
                                         : juce::String ("none"));
    report.item ("Buffer",
                 device.bufferSize > 0 ? juce::String (device.bufferSize) + " frames" : juce::String ("none"));

    if (device.message.isNotEmpty())
        report.item ("Message", device.message);
}

void writeSettings (Report& report, const DiagnosticsInput& input)
{
    // Known fields only: whatever else a settings file holds is never copied.
    report.section ("Settings");
    report.item ("Use the CPU only", input.forceCpu ? "yes" : "no");
    report.item ("Audio device", input.customAudioDevice ? "chosen by the user" : "system default");
    report.item ("Recent songs", juce::String (input.recents.size()));

    for (const auto& song : input.recents)
        report.line ("  " + song.key.substring (0, 12) + "  " + song.title + "  " + song.sourcePath);
}

void writeCache (Report& report, const std::optional<DataPaths>& paths)
{
    report.section ("Cache");

    if (! paths)
        return report.item ("Status", "unavailable");

    const CacheStore store (*paths);
    const auto overview = readCacheOverview (store);
    report.item ("Songs",
                 juce::String (overview.playableCount()) + " usable, " + juce::String (overview.unusableCount())
                     + " unusable");
    report.item ("Cache size", formatBytes (overview.usage.cacheBytes));
    report.item ("Temporary files", formatBytes (overview.usage.jobBytes));

    for (const auto& entry : overview.entries)
    {
        report.line ("  " + entry.key.substring (0, 12) + "  "
                     + SongLoadResult::statusName (entry.status).paddedRight (' ', 12) + " "
                     + formatBytes (entry.sizeBytes).paddedLeft (' ', 8) + "  "
                     + (entry.isPlayable() ? entry.song->source.fileName : entry.reason));
    }
}

void writeLogList (Report& report, const std::optional<DataPaths>& paths)
{
    report.section ("Logs");

    if (! paths)
        return report.item ("Status", "unavailable");

    const auto files = logFiles (paths);
    report.item ("Folder", paths->logsDirectory().getFullPathName());
    report.item ("Included",
                 juce::String (files.size()) + " file(s), at most " + formatBytes (logTailBytes)
                     + " from the end of each");

    for (const auto& file : files)
        report.line ("  " + file.getFileName() + "  " + describeSize (file));
}
} // namespace

juce::String diagnosticsFileName (juce::Time when)
{
    return "snds-diagnostics-" + when.formatted ("%Y%m%d-%H%M%S") + ".zip";
}

juce::String buildDiagnosticsText (const DiagnosticsInput& input, const PathRedactor& redactor, juce::Time when)
{
    Report report (redactor);
    report.line ("# SNDS diagnostics");
    report.item ("Generated", when.toISO8601 (true));

    writeApplication (report, input);
    writeSystem (report);
    writeDataDirectories (report, input.paths);
    writeModel (report, input.paths);
    writeWorker (report, input);
    writeRenderer (report, input);
    writeDevice (report, input);
    writeSettings (report, input);
    writeCache (report, input.paths);
    writeLogList (report, input.paths);
    return report.text();
}

juce::Result writeDiagnosticsZip (const DiagnosticsInput& input,
                                  const PathRedactor& redactor,
                                  const juce::File& zipFile,
                                  juce::Time when)
{
    juce::ZipFile::Builder builder;

    const auto add = [&] (const juce::String& name, const juce::String& text)
    {
        builder.addEntry (std::make_unique<juce::MemoryInputStream> (text.toRawUTF8(), text.getNumBytesAsUTF8(), true),
                          6,
                          name,
                          when);
    };

    add ("diagnostics.txt", buildDiagnosticsText (input, redactor, when));

    for (const auto& file : logFiles (input.paths))
        add ("logs/" + file.getFileName(), redactor.redact (readTail (file)));

    if (const auto created = zipFile.getParentDirectory().createDirectory(); created.failed())
        return created;

    const juce::TemporaryFile temporary (zipFile);
    {
        juce::FileOutputStream output (temporary.getFile());

        if (! output.openedOk())
            return juce::Result::fail ("Could not write " + temporary.getFile().getFullPathName() + ": "
                                       + output.getStatus().getErrorMessage());

        output.setPosition (0);
        output.truncate();

        if (! builder.writeToStream (output, nullptr))
            return juce::Result::fail ("Could not write the zip file.");
    }

    if (! temporary.overwriteTargetFileWithTemporary())
        return juce::Result::fail ("Could not save " + zipFile.getFullPathName() + ".");

    return juce::Result::ok();
}
} // namespace snds
