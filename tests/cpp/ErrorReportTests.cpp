#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "FakeAudioDevice.h"
#include "TestHelpers.h"
#include "app/ErrorReport.h"
#include "ui/ErrorBanner.h"
#include "ui/ImportPanel.h"
#include "ui/TransportBar.h"

#include <set>

namespace snds
{
namespace
{
using namespace snds::test;

bool endsWithDismiss (const ErrorReport& report)
{
    return ! report.actions.empty() && report.actions.back().kind == RecoveryKind::dismiss;
}

bool hasActionOtherThanDismiss (const ErrorReport& report)
{
    for (const auto& action : report.actions)
        if (action.kind != RecoveryKind::dismiss)
            return true;

    return false;
}

SessionStatus failedStatus (const juce::String& code, bool canRetry = false, bool canRetryOnCpu = false)
{
    SessionStatus status;
    status.state = SessionState::failed;
    status.headline = "Something failed (" + code + ").";
    status.detail = "The reason.";
    status.failureCode = code;
    status.failureDetails = "traceback line 1\ntraceback line 2";
    status.canRetry = canRetry;
    status.canRetryOnCpu = canRetryOnCpu;
    return status;
}

class ErrorReportTests final : public juce::UnitTest
{
public:
    ErrorReportTests()
        : juce::UnitTest ("ErrorReport", "snds")
    {
    }

    void runTest() override
    {
        testCatalogue();
        testReadiness();
        testSession();
        testDeviceAndPlayback();
        testOriginal();
        testRenderer();
        testText();
    }

private:
    void testCatalogue()
    {
        beginTest ("ErrorReport: every code the worker protocol defines has a next step, an area and a recovery");
        const auto known = knownFailureCodes();

        for (const auto* code : { "invalid_request",
                                  "unsupported_protocol_version",
                                  "source_unreadable",
                                  "source_changed",
                                  "source_unsupported",
                                  "decode_failed",
                                  "model_missing",
                                  "cuda_failed",
                                  "out_of_memory",
                                  "separation_failed",
                                  "normalization_failed",
                                  "validation_failed",
                                  "finalize_failed",
                                  "cache_conflict",
                                  "disk_full",
                                  "internal_error" })
            expect (std::find (known.begin(), known.end(), juce::String (code)) != known.end(),
                    juce::String (code) + " is in the catalogue");

        // Codes the app itself raises (SongSession), so none shows a bare message.
        for (const auto* code : { "import_rejected",
                                  "fingerprint_failed",
                                  "cache_remove_failed",
                                  "work_folder",
                                  "launch_failed",
                                  "published_invalid",
                                  "timeout",
                                  "worker_crashed",
                                  "unsupported_protocol",
                                  "cache_missing",
                                  "cache_incompatible",
                                  "cache_corrupt",
                                  "locate_mismatch" })
            expect (std::find (known.begin(), known.end(), juce::String (code)) != known.end(),
                    juce::String (code) + " is in the catalogue");

        std::set<juce::String> unique (known.begin(), known.end());
        expectEquals (static_cast<int> (unique.size()), static_cast<int> (known.size()), "no duplicate codes");

        for (const auto& code : known)
        {
            expect (guidanceForCode (code).isNotEmpty(), code + ": guidance");
            const auto report = reportFromSession (failedStatus (code));
            expect (report.has_value(), code);

            if (report)
            {
                expect (hasActionOtherThanDismiss (*report), code + ": offers something to do");
                expect (endsWithDismiss (*report), code + ": ends with Dismiss");
                expect (report->guidance.isNotEmpty(), code + ": says what to do next");
                expect (report->hasDetails(), code + ": details are there to expand");
                expect (report->area == areaForCode (code));

                for (const auto& action : report->actions)
                    expect (action.label.isNotEmpty());
            }
        }

        expectEquals (guidanceForCode ("something_new"), juce::String());
        expectEquals (guidanceForCode ("something_new", "retry"),
                      juce::String ("Something briefly held the output folder. Trying again usually works."));
        expect (areaForCode ("cuda_failed") == FailureArea::worker
                && areaForCode ("model_missing") == FailureArea::environment
                && areaForCode ("import_rejected") == FailureArea::importFile
                && areaForCode ("cache_corrupt") == FailureArea::cache);
    }

    void testReadiness()
    {
        beginTest ("ErrorReport: a worker environment that is not ready becomes a report with Re-check");
        Readiness ready;
        ready.state = Readiness::State::ready;
        expect (! reportFromReadiness (ready).has_value());
        Readiness checking;
        expect (! reportFromReadiness (checking).has_value());

        for (const auto state : { Readiness::State::notSetUp, Readiness::State::modelMissing, Readiness::State::error })
        {
            Readiness readiness;
            readiness.state = state;
            readiness.headline = "Headline";
            readiness.guidance = "Run scripts\\bootstrap.ps1, then press Re-check.";
            readiness.details = "Python: missing";
            const auto report = reportFromReadiness (readiness);
            expect (report.has_value());

            if (report)
            {
                expect (report->area == FailureArea::environment && report->severity == Severity::error);
                expectEquals (report->message, juce::String ("Headline"));
                expectEquals (report->guidance, readiness.guidance);
                expectEquals (report->details, readiness.details);
                expect (report->hasAction (RecoveryKind::recheck));
            }
        }
    }

    void testSession()
    {
        beginTest ("ErrorReport: a failed session offers the recovery its failure allows, in order, ending in Dismiss");
        auto report = reportFromSession (failedStatus ("cuda_failed", false, true));
        expect (report.has_value());
        expect (report->actions.size() == 2 && report->actions[0].kind == RecoveryKind::retryOnCpu
                && report->actions[1].kind == RecoveryKind::dismiss);
        expect (report->details.startsWith ("The reason.") && report->details.contains ("traceback line 2"),
                report->details);

        report = reportFromSession (failedStatus ("finalize_failed", true, false));
        expect (report->actions.front().kind == RecoveryKind::retry);
        expectEquals (report->actions.front().label, juce::String ("Try again"));

        report = reportFromSession (failedStatus ("timeout", true, true));
        expect (report->actions[0].kind == RecoveryKind::retry && report->actions[1].kind == RecoveryKind::retryOnCpu);

        report = reportFromSession (failedStatus ("import_rejected"));
        expect (report->hasAction (RecoveryKind::chooseAnotherFile) && report->area == FailureArea::importFile);

        report = reportFromSession (failedStatus ("launch_failed"));
        expect (report->hasAction (RecoveryKind::recheck) && report->area == FailureArea::environment);

        report = reportFromSession (failedStatus ("locate_mismatch"));
        expect (report->hasAction (RecoveryKind::locateOriginal));

        report = reportFromSession (failedStatus ("cache_corrupt"));
        expectEquals (report->actions.front().label, juce::String ("Import the song again"));

        report = reportFromSession (failedStatus ("worker_crashed", true));
        expect (report->hasAction (RecoveryKind::retry) && report->hasAction (RecoveryKind::openLogs));

        report = reportFromSession (failedStatus ("a_code_from_the_future"));
        expect (report.has_value() && report->hasAction (RecoveryKind::chooseAnotherFile) && endsWithDismiss (*report),
                "an unknown code still has a way forward");

        SessionStatus idle;
        expect (! reportFromSession (idle).has_value());
        idle.state = SessionState::preparing;
        expect (! reportFromSession (idle).has_value());

        // The session itself takes its next-step text from the same catalogue.
        auto status = failedStatus ("model_missing");
        status.guidance = {};
        expectEquals (reportFromSession (status)->guidance, guidanceForCode ("model_missing"));
    }

    void testDeviceAndPlayback()
    {
        beginTest ("ErrorReport: device and playback problems explain the state, keep the position, and offer settings "
                   "or reconnect");
        using State = audio::DeviceStatus::State;
        audio::DeviceStatus device;
        device.state = State::running;
        expect (! reportFromDevice (device, true).has_value());

        device.state = State::fallback;
        device.deviceName = "Speakers";
        device.preferredDeviceName = "Headphones";
        device.message = "Headphones is not available; using Speakers.";
        auto report = reportFromDevice (device, true);
        expect (report->severity == Severity::warning && report->code == "device_fallback");
        expect (report->guidance.contains ("Headphones") && report->hasAction (RecoveryKind::reconnectDevice)
                && report->hasAction (RecoveryKind::openAudioSettings));

        device.state = State::lost;
        device.deviceName = {};
        device.message = "Headphones is not available.";
        report = reportFromDevice (device, true);
        expect (report->severity == Severity::error && report->code == "device_lost"
                && report->guidance.contains ("position is kept"));

        device.state = State::failed;
        device.message = {};
        report = reportFromDevice (device, false);
        expect (report->code == "device_failed" && report->message.isNotEmpty());

        device.state = State::notStarted;
        expect (! reportFromDevice (device, false).has_value(), "no song, no noise");
        report = reportFromDevice (device, true);
        expect (report->code == "device_not_started" && report->hasAction (RecoveryKind::openAudioSettings));

        PlaybackView view;
        view.device.state = State::running;
        expect (! reportFromPlayback (view).has_value());
        view.state = PlaybackState::failed;
        view.message = "drums.wav has 100 frames; the song has 200.";
        report = reportFromPlayback (view);
        expect (report->area == FailureArea::playback && report->code == "playback_load_failed"
                && report->message.contains ("drums.wav"));
        expectEquals (report->actions.front().label, juce::String ("Import the song again"));
        expect (report->hasAction (RecoveryKind::openLogs));

        view.state = PlaybackState::paused;
        view.device.state = State::lost;
        expect (reportFromPlayback (view)->code == "device_lost");
    }

    void testOriginal()
    {
        beginTest ("ErrorReport: a missing or mismatched original is a warning that offers Locate");
        OriginalAvailability availability;
        expect (! reportFromOriginal (availability).has_value());
        availability.kind = OriginalAvailability::Kind::available;
        expect (! reportFromOriginal (availability).has_value());
        availability.kind = OriginalAvailability::Kind::missing;
        auto report = reportFromOriginal (availability);
        expect (report->severity == Severity::warning && report->hasAction (RecoveryKind::locateOriginal)
                && report->message.contains ("not found"));
        availability.kind = OriginalAvailability::Kind::unusable;
        availability.reason = "offset by 300 samples";
        report = reportFromOriginal (availability);
        expect (report->code == "original_unusable" && report->details == "offset by 300 samples"
                && report->guidance.contains ("stems still play"));
    }

    void testRenderer()
    {
        beginTest (
            "ErrorReport: every renderer fallback reads clearly, says audio is unaffected, and can be dismissed");

        for (const auto* cause : { "disabled",
                                   "context-creation-failed",
                                   "below-minimum-version",
                                   "shader-compile-failed",
                                   "resource-setup-failed",
                                   "context-lost",
                                   "watchdog-timeout",
                                   "cpu-over-budget",
                                   "unavailable" })
            for (const auto* to : { "cpu", "static" })
            {
                const auto report = reportFromRenderer ({ "gl", to, cause, "driver said: oops" });
                expect (report.area == FailureArea::renderer && report.message.isNotEmpty(), cause);
                expect (endsWithDismiss (report));
                expect (report.details.contains (cause) && report.details.contains ("driver said: oops"));
                expect (report.severity == Severity::info || report.severity == Severity::warning);

                if (juce::String (cause) != "disabled")
                    expect (report.guidance.contains ("Audio is not affected"), cause);
            }

        expect (reportFromRenderer ({ "gl", "static", "context-lost", {} }).message.contains ("still artwork"));
        expect (
            reportFromRenderer ({ "gl", "cpu", "shader-compile-failed", {} }).message.contains ("lighter animation"));
        expect (reportFromRenderer ({ "gl", "cpu", "disabled", {} }).severity == Severity::info);
    }

    void testText()
    {
        beginTest ("ErrorReport: the plain-text form carries area, code, next step, actions and details");
        const auto text = reportFromSession (failedStatus ("cuda_failed", false, true))->toText();
        expect (text.contains ("worker / error / cuda_failed") && text.contains ("Next:")
                    && text.contains ("Retry on CPU") && text.contains ("traceback line 1"),
                text);
    }
};

ErrorReportTests errorReportTests;

//==============================================================================
class ErrorUiTests final : public juce::UnitTest
{
public:
    ErrorUiTests()
        : juce::UnitTest ("ErrorUi", "snds")
    {
    }

    void runTest() override
    {
        MessageLoop loop;
        testBanner();
        testImportPanel();
        testTransportBar();
        testSnapshots();
    }

private:
    void testBanner()
    {
        beginTest ("ErrorUi: the banner shows message, guidance, one button per action, and expandable details");
        ErrorBanner banner;
        banner.setSize (700, 200);
        expectEquals (banner.getPreferredHeight(), 0);
        expect (! banner.isVisible());

        int changes = 0;
        banner.onPreferredHeightChanged = [&] { ++changes; };
        std::vector<RecoveryKind> pressed;
        banner.onAction = [&] (RecoveryKind kind) { pressed.push_back (kind); };

        banner.setReport (reportFromSession (failedStatus ("cuda_failed", false, true)));
        expect (banner.isVisible() && banner.getPreferredHeight() > 60);
        expectEquals (banner.messageLabel().getText(), juce::String ("Something failed (cuda_failed)."));
        expect (banner.guidanceLabel().getText().contains ("Retry on the CPU"));
        expect (banner.findActionButton (RecoveryKind::retryOnCpu) != nullptr
                && banner.findActionButton (RecoveryKind::dismiss) != nullptr);
        expect (banner.findActionButton (RecoveryKind::retry) == nullptr);
        expect (changes >= 1);

        const auto collapsed = banner.getPreferredHeight();
        expect (! banner.areDetailsShown() && banner.detailsToggle().isVisible());
        banner.detailsToggle().triggerClick();
        MessageLoop::pump (40);
        expect (banner.areDetailsShown() && banner.getPreferredHeight() > collapsed + 100);

        banner.findActionButton (RecoveryKind::retryOnCpu)->triggerClick();
        MessageLoop::pump (40);
        banner.findActionButton (RecoveryKind::dismiss)->triggerClick();
        MessageLoop::pump (40);
        expect (pressed.size() == 2 && pressed[0] == RecoveryKind::retryOnCpu && pressed[1] == RecoveryKind::dismiss);

        banner.setReport (std::nullopt);
        expect (! banner.isVisible() && banner.getPreferredHeight() == 0);

        // A report without details shows no Details button; each new report starts collapsed.
        auto plain = reportFromRenderer ({ "gl", "cpu", "disabled", {} });
        plain.details = {};
        banner.setReport (plain);
        expect (! banner.detailsToggle().isVisible());
    }

    void testImportPanel()
    {
        beginTest ("ErrorUi: a failed import or preparation shows the shared banner and reports the chosen recovery");
        ImportPanel panel;
        panel.setSize (900, 500);
        std::vector<RecoveryKind> pressed;
        panel.onRecovery = [&] (RecoveryKind kind) { pressed.push_back (kind); };

        auto status = failedStatus ("cuda_failed", false, true);
        panel.setStatus (status);
        auto* banner = findBanner (panel);
        expect (banner != nullptr && banner->isVisible());

        if (banner != nullptr)
        {
            expectEquals (banner->getReport()->code, juce::String ("cuda_failed"));
            banner->findActionButton (RecoveryKind::retryOnCpu)->triggerClick();
            MessageLoop::pump (40);
            expect (pressed.size() == 1 && pressed[0] == RecoveryKind::retryOnCpu);
        }

        SessionStatus ok;
        ok.headline = "Ready";
        panel.setStatus (ok);
        expect (banner != nullptr && ! banner->isVisible(), "no failure, no banner");
    }

    void testTransportBar()
    {
        beginTest (
            "ErrorUi: a song that cannot be loaded shows the problem and its first recovery on the transport bar");
        ScopedTempDirectory empty;
        DispatchQueue queue;
        audio::PlaybackEngine engine;
        engine.prepare (44100.0, 512);
        PlaybackController controller (engine, nullptr, queue.dispatcher());

        OpenSong song; // a package whose stem files are not there
        song.entry.key = "broken-1";
        song.entry.directory = empty.get();
        song.entry.song = SongMetadata {};
        song.entry.song->source.frames = 1000;
        song.entry.song->source.sampleRate = 44100;

        for (size_t i = 0; i < song.entry.song->stems.size(); ++i)
            song.entry.song->stems[i].file = juce::String (dsp::stemIds[i].data(), dsp::stemIds[i].size()) + ".wav";

        controller.setSong (song);
        expect (queue.pumpUntil ([&] { return controller.view().state == PlaybackState::failed; }));

        TransportBar bar;
        bar.setSize (900, TransportBar::preferredHeight);
        std::vector<RecoveryKind> pressed;
        bar.onRecovery = [&] (RecoveryKind kind) { pressed.push_back (kind); };
        bar.setController (&controller);
        bar.refresh();

        expect (bar.getReport().has_value() && bar.getReport()->code == "playback_load_failed");
        expect (bar.fixButton().isVisible() && bar.fixButton().getButtonText() == "Import the song again");
        bar.fixButton().triggerClick();
        MessageLoop::pump (40);
        expect (pressed.size() == 1 && pressed[0] == RecoveryKind::chooseAnotherFile);

        bar.setController (nullptr);
        bar.refresh();
        expect (! bar.getReport().has_value() && ! bar.fixButton().isVisible(), "nothing wrong, nothing shown");
    }

    void testSnapshots()
    {
        beginTest ("ErrorUi: images of a failed preparation and a lost device (opt-in: SNDS_SNAPSHOT_DIR)");
        const auto directory = juce::SystemStats::getEnvironmentVariable ("SNDS_SNAPSHOT_DIR", {});

        if (directory.isEmpty())
            return;

        ImportPanel panel;
        panel.setSize (900, 330);
        auto status = failedStatus ("cuda_failed", false, true);
        status.headline = "Preparing song.wav failed.";
        status.detail = "CUDA out of memory while separating.";
        panel.setStatus (status);
        save (panel, juce::File (directory).getChildFile ("error-import.png"));

        if (auto* banner = findBanner (panel))
            banner->setDetailsShown (true);

        save (panel, juce::File (directory).getChildFile ("error-import-details.png"));
    }

    static void save (juce::Component& component, const juce::File& file)
    {
        const auto snapshot = component.createComponentSnapshot (component.getLocalBounds(), true, 1.0f);
        juce::Image image (juce::Image::RGB, component.getWidth(), component.getHeight(), true);

        {
            juce::Graphics g (image);
            g.fillAll (juce::Colour (0xff323e44));
            g.drawImageAt (snapshot, 0, 0);
        }

        juce::PNGImageFormat png;
        file.deleteFile();
        juce::FileOutputStream out (file);

        if (out.openedOk())
            png.writeImageToStream (image, out);
    }

    static ErrorBanner* findBanner (juce::Component& parent)
    {
        for (auto* child : parent.getChildren())
            if (auto* banner = dynamic_cast<ErrorBanner*> (child))
                return banner;

        return nullptr;
    }
};

ErrorUiTests errorUiTests;
} // namespace
} // namespace snds
