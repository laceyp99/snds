#include <juce_core/juce_core.h>

#include "SongFixtures.h"
#include "TestHelpers.h"
#include "app/CacheStore.h"
#include "app/DiagnosticsExport.h"
#include "app/ErrorReport.h"

namespace snds
{
namespace
{
using namespace snds::test;

juce::String readEntry (juce::ZipFile& zip, const juce::String& name)
{
    const auto index = zip.getIndexOfFileName (name);

    if (index < 0)
        return "<missing " + name + ">";

    std::unique_ptr<juce::InputStream> stream (zip.createStreamForEntry (index));
    return stream != nullptr ? stream->readEntireStreamAsString() : juce::String ("<unreadable>");
}

class DiagnosticsExportTests final : public juce::UnitTest
{
public:
    DiagnosticsExportTests()
        : juce::UnitTest ("DiagnosticsExport", "snds")
    {
    }

    void runTest() override
    {
        testContents();
        testRedaction();
        testActions();
    }

private:
    // A fake user profile inside a temp folder, so redaction is tested without the real one.
    struct Rig
    {
        ScopedTempDirectory temp;
        juce::File profile = temp.child ("Users\\Someone");
        DataPaths paths { profile.getChildFile ("AppData\\Local\\snds") };
        PathRedactor redactor { { { "USERPROFILE", profile.getFullPathName() },
                                  { "LOCALAPPDATA", profile.getChildFile ("AppData\\Local").getFullPathName() } } };

        DiagnosticsInput input() const
        {
            DiagnosticsInput result;
            result.paths = paths;
            result.worker.root = profile.getChildFile ("code\\snds\\worker");
            result.worker.source = "build default";
            result.readiness.state = Readiness::State::ready;
            result.readiness.headline = "Worker ready";
            result.readiness.details =
                "worker: 0.0.1\nprotocol: 1\ntorch: 2.1.2+cpu\npython: "
                + profile.getChildFile ("code\\snds\\worker\\.venv\\Scripts\\python.exe").getFullPathName();
            result.recents = {
                { "abcdef0123456789", "song.wav", profile.getChildFile ("Music\\song.wav").getFullPathName() }
            };
            result.rendererStatus = "Visuals: GPU (OpenGL 3.2)";
            audio::DeviceStatus device;
            device.state = audio::DeviceStatus::State::running;
            device.deviceName = "Speakers";
            device.preferredDeviceName = "Speakers";
            device.sampleRate = 48000.0;
            device.bufferSize = 512;
            result.device = device;
            return result;
        }
    };

    void testContents()
    {
        beginTest ("DiagnosticsExport: the zip holds the report and the recent logs, and every section the script has");
        Rig rig;
        rig.paths.logsDirectory().createDirectory();
        rig.paths.logsDirectory().getChildFile ("app.log").replaceWithText ("line one\nline two\n");
        rig.paths.logsDirectory().getChildFile ("worker.log").replaceWithText ("worker line\n");
        SongFixture().write (rig.paths.cacheDirectory());
        SongFixture broken;
        broken.sha256 = juce::String::repeatedString ("cd", 32);
        broken.write (rig.paths.cacheDirectory(), [] (juce::DynamicObject& d) { d.setProperty ("cacheSchema", 2); });

        const auto zipFile = rig.temp.child ("out\\diag.zip");
        const auto written = writeDiagnosticsZip (rig.input(), rig.redactor, zipFile, juce::Time::getCurrentTime());
        expect (written.wasOk(), written.getErrorMessage());
        expect (zipFile.existsAsFile());

        juce::ZipFile zip (zipFile);
        const auto report = readEntry (zip, "diagnostics.txt");

        for (const auto* section : { "Application",
                                     "System",
                                     "Data directories",
                                     "Model",
                                     "Worker environment",
                                     "Renderer",
                                     "Audio device",
                                     "Settings",
                                     "Cache",
                                     "Logs" })
            expect (report.contains (juce::String ("## ") + section), section);

        expect (report.contains ("protocol: 1"), "the readiness output is included");
        expect (report.contains ("Visuals: GPU"), "the renderer status is included");
        expect (report.contains ("48000 Hz") && report.contains ("Speakers"), "the device status is included");
        expect (report.contains ("1 usable, 1 unusable"), "the cache is summarised");
        expect (report.contains ("song.wav"), "the song is named");
        expect (readEntry (zip, "logs/app.log").contains ("line two"));
        expect (readEntry (zip, "logs/worker.log").contains ("worker line"));
        expect (! zipFile.getParentDirectory().findChildFiles (juce::File::findFiles, false, "*.tmp").size(),
                "no temporary file is left behind");
    }

    void testRedaction()
    {
        beginTest ("DiagnosticsExport: user paths are replaced everywhere, and settings hold only known fields");
        Rig rig;
        rig.paths.logsDirectory().createDirectory();
        rig.paths.logsDirectory().getChildFile ("app.log").replaceWithText (
            "opened " + rig.profile.getChildFile ("Music\\song.wav").getFullPathName() + "\n");

        // A settings file with a field the export has never heard of must not be copied.
        rig.paths.settingsFile().replaceWithText (
            R"({"format":"snds-settings","version":1,"apiToken":"s3cr3t-value","forceCpu":true})");

        const auto zipFile = rig.temp.child ("diag.zip");
        expect (writeDiagnosticsZip (rig.input(), rig.redactor, zipFile, juce::Time::getCurrentTime()).wasOk());

        juce::ZipFile zip (zipFile);
        const auto user = rig.profile.getFullPathName();

        for (int i = 0; i < zip.getNumEntries(); ++i)
        {
            const auto entryName = zip.getEntry (i)->filename;
            const auto text = readEntry (zip, entryName);
            expect (! text.containsIgnoreCase (user), entryName + " still names the user folder");
            expect (! text.containsIgnoreCase ("Someone"), entryName + " still names the user");
            expect (! text.contains ("s3cr3t-value"), entryName + " leaked a settings value");
        }

        const auto report = readEntry (zip, "diagnostics.txt");
        expect (report.contains ("%LOCALAPPDATA%\\snds"), "the data folder reads %LOCALAPPDATA%");
        expect (report.contains ("%USERPROFILE%"), "the worker and recent paths read %USERPROFILE%");
        expect (readEntry (zip, "logs/app.log").contains ("%USERPROFILE%\\Music\\song.wav"));
    }

    void testActions()
    {
        beginTest ("DiagnosticsExport: failures that point at the logs also offer to export diagnostics");
        SessionStatus status;
        status.state = SessionState::failed;
        status.failureCode = "separation_failed";
        status.headline = "Separation failed.";
        const auto report = reportFromSession (status);
        expect (report.has_value() && report->hasAction (RecoveryKind::openLogs));
        expect (report.has_value() && report->hasAction (RecoveryKind::exportDiagnostics));
        expect (report.has_value() && report->actions.back().kind == RecoveryKind::dismiss, "Dismiss stays last");

        status.failureCode = "cache_conflict";
        const auto plain = reportFromSession (status);
        expect (plain.has_value() && ! plain->hasAction (RecoveryKind::exportDiagnostics),
                "no export where the logs would not help");
    }
};

DiagnosticsExportTests diagnosticsExportTests;
} // namespace
} // namespace snds
