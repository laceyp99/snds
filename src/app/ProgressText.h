#pragma once

#include "app/SongSession.h"

#include <optional>

namespace snds
{
/** "45 s", "1 min 20 s", "2 h 05 min": a span in the shortest honest words. Spans over a minute
    are rounded to 5 seconds, over an hour to the minute. */
juce::String formatSpan (double seconds);

/** The worker's estimate for the current step if it is worth showing: present, finite, between
    1 second and 6 hours, and only while the job is really running (not cancelling). The worker
    only offers one once it can measure (separation after 5 % and 5 s), and this is an estimate
    for that step, not for the whole job, so it is never presented as a total. */
std::optional<double> credibleEta (const SessionStatus& status);

/** The line under the progress bar: the step ("Separating stems (2 of 6)"), the step's estimate
    when there is a credible one ("about 1 min 20 s left in this step"), the device, and the time
    elapsed since preparation began. Nothing is invented: no estimate, no estimate text. */
juce::String describeProgress (const SessionStatus& status, double elapsedSeconds);

/** "Prepared in 2 min 10 s on the GPU." for the ready notification. */
juce::String describePrepared (double elapsedSeconds, const juce::String& device);
} // namespace snds
