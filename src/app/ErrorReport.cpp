#include "app/ErrorReport.h"

#include <algorithm>

namespace snds
{
namespace
{
struct CodeEntry
{
    const char* code;
    FailureArea area;
    const char* guidance;
    std::vector<RecoveryKind> extraActions; // beyond Try again / Retry on CPU / Dismiss, which come from the session
};

const std::vector<CodeEntry>& catalogue()
{
    static const std::vector<CodeEntry> entries = {
        // Environment: the worker cannot run.
        { "model_missing",
          FailureArea::environment,
          "Run scripts\\bootstrap.ps1 to install the model, then press Re-check.",
          { RecoveryKind::recheck } },
        { "launch_failed",
          FailureArea::environment,
          "Run scripts\\bootstrap.ps1, then press Re-check.",
          { RecoveryKind::recheck } },
        { "unsupported_protocol",
          FailureArea::environment,
          "The worker and the app are different versions. Run scripts\\bootstrap.ps1 to update the worker, then press "
          "Re-check.",
          { RecoveryKind::recheck } },
        { "unsupported_protocol_version",
          FailureArea::environment,
          "The worker and the app are different versions. Run scripts\\bootstrap.ps1 to update the worker, then press "
          "Re-check.",
          { RecoveryKind::recheck } },
        { "work_folder",
          FailureArea::environment,
          "Check that the SNDS data folder can be written to and has free space, then try again.",
          { RecoveryKind::openLogs } },

        // Import: the file.
        { "import_rejected",
          FailureArea::importFile,
          "Choose a standard WAV or MP3 file.",
          { RecoveryKind::chooseAnotherFile } },
        { "fingerprint_failed",
          FailureArea::importFile,
          "Check that the file still exists and is not open in another program, then import it again.",
          { RecoveryKind::chooseAnotherFile } },
        { "source_unreadable",
          FailureArea::importFile,
          "Check that the file still exists and is not open in another program, then import it again.",
          { RecoveryKind::chooseAnotherFile } },
        { "source_changed",
          FailureArea::importFile,
          "The file changed after it was opened. Import it again.",
          { RecoveryKind::chooseAnotherFile } },
        { "source_unsupported",
          FailureArea::importFile,
          "Try exporting the song again as a standard PCM WAV file.",
          { RecoveryKind::chooseAnotherFile } },
        { "decode_failed",
          FailureArea::importFile,
          "Try exporting the song again as a standard PCM WAV file.",
          { RecoveryKind::chooseAnotherFile } },

        // Worker: separation and analysis.
        { "cuda_failed",
          FailureArea::worker,
          "The GPU could not finish. Retry on the CPU, which is slower but does not need the GPU.",
          {} },
        { "out_of_memory",
          FailureArea::worker,
          "The GPU could not finish. Retry on the CPU, which is slower but does not need the GPU.",
          {} },
        { "disk_full",
          FailureArea::worker,
          "Free some disk space (Windows Storage settings can help), then try again.",
          {} },
        { "timeout", FailureArea::worker, "Try again, or retry on the CPU if the GPU may be stalling.", {} },
        { "worker_crashed",
          FailureArea::worker,
          "The worker stopped unexpectedly. Try again; the logs hold what it printed.",
          { RecoveryKind::openLogs } },
        { "separation_failed",
          FailureArea::worker,
          "Try again. If it keeps failing, the logs hold the details.",
          { RecoveryKind::openLogs } },
        { "normalization_failed",
          FailureArea::worker,
          "Try again, or export the song again as a standard PCM WAV file.",
          { RecoveryKind::openLogs } },
        { "validation_failed",
          FailureArea::worker,
          "The result did not pass its checks and was not kept. Try again.",
          { RecoveryKind::openLogs } },
        { "finalize_failed",
          FailureArea::worker,
          "Something briefly held the output folder. Trying again usually works.",
          {} },
        { "internal_error",
          FailureArea::worker,
          "Something unexpected went wrong. Try again; the logs hold the details.",
          { RecoveryKind::openLogs } },
        { "invalid_request",
          FailureArea::worker,
          "The worker rejected the request. Re-check the environment, then try again.",
          { RecoveryKind::recheck, RecoveryKind::openLogs } },

        // Cache: stored songs.
        { "cache_conflict", FailureArea::cache, "Press Try again to prepare the song.", {} },
        { "cache_missing",
          FailureArea::cache,
          "Import the original file again; SNDS will prepare it again.",
          { RecoveryKind::chooseAnotherFile } },
        { "cache_incompatible",
          FailureArea::cache,
          "This song was prepared by a different version. Import the original file again.",
          { RecoveryKind::chooseAnotherFile } },
        { "cache_corrupt",
          FailureArea::cache,
          "The stored files are damaged. Import the original file again.",
          { RecoveryKind::chooseAnotherFile } },
        { "cache_remove_failed",
          FailureArea::cache,
          "Close any program using SNDS files, then try again.",
          { RecoveryKind::chooseAnotherFile } },
        { "published_invalid",
          FailureArea::cache,
          "The result could not be used. Try again; the logs hold the details.",
          { RecoveryKind::openLogs } },
        { "locate_mismatch",
          FailureArea::cache,
          "Choose the exact file this song was made from.",
          { RecoveryKind::locateOriginal } },
    };

    return entries;
}

const CodeEntry* find (const juce::String& code)
{
    for (const auto& entry : catalogue())
        if (code == entry.code)
            return &entry;

    return nullptr;
}

juce::String labelFor (RecoveryKind kind, FailureArea area)
{
    switch (kind)
    {
        case RecoveryKind::recheck:
            return "Re-check";
        case RecoveryKind::retry:
            return "Try again";
        case RecoveryKind::retryOnCpu:
            return "Retry on CPU";
        case RecoveryKind::chooseAnotherFile:
            return area == FailureArea::cache ? "Import the song again" : "Choose another file";
        case RecoveryKind::locateOriginal:
            return "Locate original...";
        case RecoveryKind::openAudioSettings:
            return "Audio settings...";
        case RecoveryKind::reconnectDevice:
            return "Reconnect";
        case RecoveryKind::openLogs:
            return "Open logs folder";
        case RecoveryKind::exportDiagnostics:
            return "Export diagnostics...";
        case RecoveryKind::dismiss:
            return "Dismiss";
    }

    return {};
}

void addAction (ErrorReport& report, RecoveryKind kind)
{
    if (! report.hasAction (kind))
        report.actions.push_back ({ kind, labelFor (kind, report.area) });
}

juce::String describeDevice (const audio::DeviceStatus& device)
{
    juce::StringArray lines;
    lines.add ("Device: " + (device.deviceName.isEmpty() ? juce::String ("none") : device.deviceName));

    if (device.typeName.isNotEmpty())
        lines.add ("Type: " + device.typeName);

    if (device.preferredDeviceName.isNotEmpty())
        lines.add ("Preferred: " + device.preferredDeviceName);

    if (device.sampleRate > 0.0)
        lines.add ("Rate: " + juce::String (juce::roundToInt (device.sampleRate)) + " Hz, buffer "
                   + juce::String (device.bufferSize) + " frames");

    if (device.message.isNotEmpty())
        lines.add ("Message: " + device.message);

    return lines.joinIntoString ("\n");
}
} // namespace

//==============================================================================
bool ErrorReport::hasAction (RecoveryKind kind) const noexcept
{
    return std::any_of (
        actions.begin(), actions.end(), [kind] (const RecoveryAction& action) { return action.kind == kind; });
}

juce::String ErrorReport::areaName (FailureArea area)
{
    switch (area)
    {
        case FailureArea::environment:
            return "environment";
        case FailureArea::importFile:
            return "import";
        case FailureArea::worker:
            return "worker";
        case FailureArea::cache:
            return "cache";
        case FailureArea::playback:
            return "playback";
        case FailureArea::device:
            return "device";
        case FailureArea::renderer:
            return "renderer";
    }

    return {};
}

juce::String ErrorReport::severityName (Severity severity)
{
    return severity == Severity::error ? "error" : severity == Severity::warning ? "warning" : "info";
}

juce::String ErrorReport::toText() const
{
    juce::StringArray lines;
    lines.add ("[" + areaName (area) + " / " + severityName (severity) + " / " + code + "] " + message);

    if (guidance.isNotEmpty())
        lines.add ("Next: " + guidance);

    if (! actions.empty())
    {
        juce::StringArray labels;

        for (const auto& action : actions)
            labels.add (action.label);

        lines.add ("Actions: " + labels.joinIntoString (", "));
    }

    if (details.isNotEmpty())
        lines.add ("Details:\n" + details);

    return lines.joinIntoString ("\n");
}

//==============================================================================
juce::String guidanceForCode (const juce::String& code, const juce::String& suggestedAction)
{
    if (const auto* entry = find (code))
        return entry->guidance;

    if (suggestedAction == "retry")
        return "Something briefly held the output folder. Trying again usually works.";

    return {};
}

FailureArea areaForCode (const juce::String& code)
{
    if (const auto* entry = find (code))
        return entry->area;

    return FailureArea::worker;
}

std::vector<juce::String> knownFailureCodes()
{
    std::vector<juce::String> codes;

    for (const auto& entry : catalogue())
        codes.push_back (entry.code);

    return codes;
}

//==============================================================================
std::optional<ErrorReport> reportFromReadiness (const Readiness& readiness)
{
    if (readiness.state == Readiness::State::ready || readiness.state == Readiness::State::checking)
        return std::nullopt;

    ErrorReport report;
    report.area = FailureArea::environment;
    report.severity = Severity::error;
    report.code = readiness.state == Readiness::State::notSetUp     ? "env_not_set_up"
                : readiness.state == Readiness::State::modelMissing ? "env_model_missing"
                                                                    : "env_error";
    report.message = readiness.headline;
    report.guidance = readiness.guidance;
    report.details = readiness.details;
    addAction (report, RecoveryKind::recheck);
    return report;
}

std::optional<ErrorReport> reportFromSession (const SessionStatus& status)
{
    if (status.state != SessionState::failed)
        return std::nullopt;

    ErrorReport report;
    report.code = status.failureCode.isNotEmpty() ? status.failureCode : "session_failed";
    report.area = areaForCode (report.code);
    report.severity = Severity::error;
    report.message = status.headline;
    report.guidance = status.guidance.isNotEmpty() ? status.guidance : guidanceForCode (report.code);

    // The detail line (often the worker's own message) leads the details; the worker's diagnostic text follows.
    juce::StringArray details;

    if (status.detail.isNotEmpty() && status.detail != status.headline)
        details.add (status.detail);

    if (status.failureDetails.isNotEmpty())
        details.add (status.failureDetails);

    report.details = details.joinIntoString ("\n\n");

    if (status.canRetry)
        addAction (report, RecoveryKind::retry);

    if (status.canRetryOnCpu)
        addAction (report, RecoveryKind::retryOnCpu);

    if (const auto* entry = find (report.code))
        for (const auto kind : entry->extraActions)
            addAction (report, kind);

    // Where the logs would help, a diagnostics zip helps more: it holds them with the context.
    if (report.hasAction (RecoveryKind::openLogs))
        addAction (report, RecoveryKind::exportDiagnostics);

    // A failure with no specific recovery still offers a way forward: pick a file again.
    if (report.actions.empty())
        addAction (report, RecoveryKind::chooseAnotherFile);

    addAction (report, RecoveryKind::dismiss);
    return report;
}

std::optional<ErrorReport> reportFromDevice (const audio::DeviceStatus& device, bool songLoaded)
{
    using State = audio::DeviceStatus::State;

    ErrorReport report;
    report.area = FailureArea::device;
    report.details = describeDevice (device);

    switch (device.state)
    {
        case State::running:
            return std::nullopt;

        case State::fallback:
            report.severity = Severity::warning;
            report.code = "device_fallback";
            report.message = device.message.isNotEmpty()
                               ? device.message
                               : "Playing on " + device.deviceName + " instead of your chosen device.";
            report.guidance =
                "You can keep listening here, or reconnect "
                + (device.preferredDeviceName.isNotEmpty() ? device.preferredDeviceName : juce::String ("your device"))
                + " and press Reconnect.";
            break;

        case State::lost:
            report.severity = Severity::error;
            report.code = "device_lost";
            report.message = device.message.isNotEmpty() ? device.message : "The audio output is not available.";
            report.guidance =
                "Playback is stopped and your position is kept. Plug the device back in, or choose another output.";
            break;

        case State::failed:
            report.severity = Severity::error;
            report.code = "device_failed";
            report.message = device.message.isNotEmpty() ? device.message : "The audio device stopped.";
            report.guidance = "Playback is stopped and your position is kept. Try Reconnect, or choose another output.";
            break;

        case State::notStarted:
            if (! songLoaded)
                return std::nullopt;

            report.severity = Severity::warning;
            report.code = "device_not_started";
            report.message = "No audio output is running.";
            report.guidance = "Open Audio settings and choose an output device.";
            break;
    }

    addAction (report, RecoveryKind::reconnectDevice);
    addAction (report, RecoveryKind::openAudioSettings);
    return report;
}

std::optional<ErrorReport> reportFromPlayback (const PlaybackView& view)
{
    if (view.state == PlaybackState::failed)
    {
        ErrorReport report;
        report.area = FailureArea::playback;
        report.severity = Severity::error;
        report.code = "playback_load_failed";
        report.message = "This song can't be played: " + view.message;
        report.guidance = "Its stored files may be damaged. Import the original file again to prepare it again.";
        report.details = view.message;
        report.area = FailureArea::playback;
        addAction (report, RecoveryKind::chooseAnotherFile);
        addAction (report, RecoveryKind::openLogs);
        addAction (report, RecoveryKind::exportDiagnostics);
        report.actions.front().label = "Import the song again";
        return report;
    }

    return reportFromDevice (view.device, view.hasSong() || view.state == PlaybackState::preparing);
}

std::optional<ErrorReport> reportFromOriginal (const OriginalAvailability& availability)
{
    ErrorReport report;
    report.area = FailureArea::cache;
    report.severity = Severity::warning;

    if (availability.kind == OriginalAvailability::Kind::missing)
    {
        report.code = "original_missing";
        report.message = "The original file was not found, so Original/Stem Mix is off.";
        report.guidance = "The stems still play. Locate the original to compare against it.";
        report.details = availability.reason;
    }
    else if (availability.kind == OriginalAvailability::Kind::unusable)
    {
        report.code = "original_unusable";
        report.message = "The original does not line up with the stems, so Original/Stem Mix is off.";
        report.guidance = "The stems still play. If this is the wrong file, locate the right one.";
        report.details = availability.reason;
    }
    else
    {
        return std::nullopt;
    }

    addAction (report, RecoveryKind::locateOriginal);
    return report;
}

ErrorReport reportFromRenderer (const RendererFallbackInfo& info)
{
    ErrorReport report;
    report.area = FailureArea::renderer;
    report.severity = Severity::warning;
    report.code =
        "renderer_" + (info.cause.isNotEmpty() ? info.cause.replaceCharacter ('-', '_') : juce::String ("fallback"));

    const auto instead = info.to.containsIgnoreCase ("static") ? juce::String ("the still artwork")
                       : info.to.containsIgnoreCase ("cpu")    ? juce::String ("a lighter animation")
                                                               : juce::String ("simpler visuals");

    if (info.cause == "disabled")
    {
        report.severity = Severity::info;
        report.message = "Animated visuals are turned off.";
    }
    else if (info.cause == "cpu-over-budget")
    {
        report.severity = Severity::info;
        report.message = "The animation was too heavy for this computer, so SNDS is showing " + instead + ".";
        report.guidance = "Audio is not affected. You can turn visual effects off in the player.";
    }
    else if (info.cause == "context-lost" || info.cause == "watchdog-timeout")
    {
        report.message = "The graphics driver stopped responding, so SNDS is showing " + instead + ".";
        report.guidance = "Audio is not affected. Updating the graphics driver may help.";
    }
    else
    {
        report.message =
            "The animated visuals are not available on this graphics setup, so SNDS is showing " + instead + ".";
        report.guidance =
            "Audio is not affected. A newer graphics driver (OpenGL 3.2 or later) enables the full animation.";
    }

    report.details = "From: " + info.from + "\nTo: " + info.to + "\nCause: " + info.cause
                   + (info.detail.isNotEmpty() ? "\n" + info.detail : juce::String());
    addAction (report, RecoveryKind::dismiss);
    return report;
}
} // namespace snds
