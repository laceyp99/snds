#pragma once

#include "app/PlaybackController.h"
#include "app/Readiness.h"
#include "app/SongSession.h"
#include "audio/AudioDeviceController.h"

#include <juce_core/juce_core.h>

#include <optional>
#include <vector>

namespace snds
{
/** Where a problem comes from. Every panel shows the same kinds of report the same way. */
enum class FailureArea
{
    environment, // the worker environment or model is missing or broken
    importFile,  // the chosen file cannot be used
    worker,      // separation or analysis failed
    cache,       // a stored song is missing, damaged, or from another version
    playback,    // a prepared song cannot be loaded or played
    device,      // the audio output
    renderer     // the animated artwork
};

enum class Severity
{
    info,    // something was simplified; nothing to do
    warning, // works, but not as intended
    error    // the user cannot continue until something is done
};

/** What the user can do about it. The panel that shows a report turns each into a button and
    carries it out; the model only names them. */
enum class RecoveryKind
{
    recheck,           // run the readiness check again
    retry,             // prepare again with the same file and artwork
    retryOnCpu,        // prepare again, forcing the CPU
    chooseAnotherFile, // pick a file to import (or import the same one again)
    locateOriginal,    // point SNDS at the original file
    openAudioSettings, // the output-device dialog
    reconnectDevice,   // try the preferred audio device again
    openLogs,          // reveal the logs folder
    exportDiagnostics, // save a diagnostics zip for a bug report
    dismiss            // clear the message
};

struct RecoveryAction
{
    RecoveryKind kind = RecoveryKind::dismiss;
    juce::String label;
};

/** One problem as the user sees it: a short message, what to do next, the actions that do it,
    and the technical details behind an "expand" (never needed to recover). */
struct ErrorReport
{
    FailureArea area = FailureArea::worker;
    Severity severity = Severity::error;
    juce::String code;                   // stable id for tests, logs and diagnostics (for example "cuda_failed")
    juce::String message;                // one sentence
    juce::String guidance;               // the next step; may be empty
    std::vector<RecoveryAction> actions; // in display order; dismissable reports end with Dismiss
    juce::String details;                // for "Show details"; may be empty

    bool hasDetails() const noexcept { return details.isNotEmpty(); }
    bool hasAction (RecoveryKind kind) const noexcept;

    /** The whole report as plain text, for logs and the diagnostics bundle. */
    juce::String toText() const;

    static juce::String areaName (FailureArea area);
    static juce::String severityName (Severity severity);
};

//==============================================================================
// The failure-code catalogue: the one place that says what each code means to the user.

/** The next step for a worker, import, cache or app failure code (empty if the code is unknown). */
juce::String guidanceForCode (const juce::String& code, const juce::String& suggestedAction = {});

/** The area a code belongs to. */
FailureArea areaForCode (const juce::String& code);

/** Every code the app can show, for tests. */
std::vector<juce::String> knownFailureCodes();

//==============================================================================
// Builders. Each returns nothing when there is no problem to show.

std::optional<ErrorReport> reportFromReadiness (const Readiness& readiness);

/** From a failed song session (import, fingerprint, cache lookup, preparation, locate). */
std::optional<ErrorReport> reportFromSession (const SessionStatus& status);

std::optional<ErrorReport> reportFromDevice (const audio::DeviceStatus& device, bool songLoaded);

/** A song that cannot be loaded, else a device problem while a song is open. */
std::optional<ErrorReport> reportFromPlayback (const PlaybackView& view);

/** Original/Stem Mix is not available: the file is missing, or does not line up with the stems. */
std::optional<ErrorReport> reportFromOriginal (const OriginalAvailability& availability);

/** A renderer step-down, as strings so the renderer need not be known here. `from` and `to` are
    the tier names ("gl", "cpu", "static", "none"), `cause` the rendering::causeName text. */
struct RendererFallbackInfo
{
    juce::String from, to, cause, detail;
};

ErrorReport reportFromRenderer (const RendererFallbackInfo& info);
} // namespace snds
