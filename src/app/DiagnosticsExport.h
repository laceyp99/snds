#pragma once

#include "app/DataPaths.h"
#include "app/Logging.h"
#include "app/Readiness.h"
#include "app/RecentSongs.h"
#include "app/WorkerLocation.h"
#include "audio/AudioDeviceController.h"

#include <optional>
#include <vector>

namespace snds
{
/** Everything the app knows that the export cannot read from disk. The message thread fills it in;
    the export itself then runs on a background thread. */
struct DiagnosticsInput
{
    std::optional<DataPaths> paths; // none: no data folder could be resolved
    WorkerLocation worker;
    Readiness readiness;

    bool forceCpu = false;
    bool customAudioDevice = false; // the user chose an output device
    std::vector<RecentSong> recents;

    juce::String rendererStatus;               // the visuals' detail line
    std::optional<audio::DeviceStatus> device; // none: the app runs without audio
};

/** "snds-diagnostics-20261006-101500.zip". */
juce::String diagnosticsFileName (juce::Time when);

/** The report as text, with the same `## Section` layout as scripts\diagnostics.ps1. Sections
    shared with the script: System, Data directories, Model, Worker environment, Settings, Cache,
    Logs. App only: Application, Renderer, Audio device. The script alone adds Source,
    Toolchain and App build. Every line goes through `redactor`, so user paths read
    %USERPROFILE% and %LOCALAPPDATA%. Reads the disk: call off the message thread. */
juce::String buildDiagnosticsText (const DiagnosticsInput& input, const PathRedactor& redactor, juce::Time when);

/** Writes the zip atomically: `diagnostics.txt` plus the tail of each log file under `logs\`
    (redacted again). Settings are written from known fields only, so nothing outside the
    whitelist can leak, and no secret is ever read. Reads the disk: call off the message thread. */
juce::Result writeDiagnosticsZip (const DiagnosticsInput& input,
                                  const PathRedactor& redactor,
                                  const juce::File& zipFile,
                                  juce::Time when);
} // namespace snds
