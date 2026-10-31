#pragma once

#include "dsp/MixParameters.h"
#include "model/SongMetadata.h"

#include <juce_core/juce_core.h>

#include <optional>

namespace snds
{
/** Writes the one thing the app may change in a finished package: `bpm.userOverride`
    (docs/protocol.md). Only that value's text in `song.json` is replaced; every other byte stays as
    the worker wrote it. The new file is written beside the old one and swapped in atomically, and
    is parsed and validated first: on any problem the original file is left alone.

    `nullopt` writes `null` (back to the detected tempo). Safe to call while another thread lists
    or reads the package, because readers see either the old or the new complete file.
*/
juce::Result writeBpmOverride (const juce::File& packageDirectory,
                               const std::optional<SongMetadata::BpmOverride>& override);
} // namespace snds
