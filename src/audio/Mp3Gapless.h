#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>

namespace snds::audio
{
/** What the first frame of an MP3 says about encoder delay and padding.

    LAME writes an "Info" (CBR) or "Xing" (VBR) frame first; its extension holds the encoder
    delay and end padding (12 bits each). The worker's decoder (libsndfile/mpg123) honours it and
    trims delay + 529 samples of decoder delay from the start. JUCE's MP3 reader does not trim
    anything, so the original it decodes starts earlier than the worker's stems.
*/
struct Mp3Info
{
    bool valid = false;        // an MPEG Layer III frame header was found
    bool hasInfoFrame = false; // a Xing/Info frame follows it
    bool hasLameTag = false;   // ...with LAME's delay/padding fields
    int sampleRate = 0;
    int channels = 0;
    int samplesPerFrame = 0;
    int encoderDelay = 0;
    int encoderPadding = 0;
    int headerFrames = 0; // frame count field of the Xing/Info frame, 0 if absent
};

/** Samples every MPEG decoder drops to compensate its own filter-bank delay. */
constexpr int standardDecoderDelay = 529;

/** Reads the leading frames of an MP3 file. Never throws; `valid` is false when unparsable. */
Mp3Info parseMp3 (const void* data, std::size_t size);
Mp3Info parseMp3 (const juce::File& file);

/** Frames the JUCE decode has before the first sample of the worker's decode: 0 when there is no
    LAME tag (neither decoder trims), else encoder delay + 529. Measured against the test
    fixture: delay 576, so the offset is 1105 (verified sample-exactly against the ideal signal). */
int predictedLeadingSkip (const Mp3Info& info);
} // namespace snds::audio
