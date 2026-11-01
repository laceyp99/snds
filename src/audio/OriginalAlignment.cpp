#include "audio/OriginalAlignment.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <complex>

namespace snds::audio
{
namespace
{
constexpr double minimumCorrelation = 0.8;
constexpr double silenceRms = 1.0e-4;

/** Reads `count` frames from `start` as mono ((left + right) / 2), summing the sources. */
bool readMono (const std::vector<FrameSource*>& sources, std::int64_t start, int count, std::vector<float>& mono)
{
    mono.assign (static_cast<size_t> (count), 0.0f);
    std::vector<float> left (static_cast<size_t> (count)), right (static_cast<size_t> (count));
    float* channels[2] = { left.data(), right.data() };
    bool ok = true;

    for (auto* source : sources)
    {
        ok = source->read (start, channels, count) && ok;

        for (int i = 0; i < count; ++i)
            mono[static_cast<size_t> (i)] += 0.5f * (left[static_cast<size_t> (i)] + right[static_cast<size_t> (i)]);
    }

    return ok;
}

double rmsOf (const std::vector<float>& values)
{
    double sum = 0.0;

    for (auto value : values)
        sum += static_cast<double> (value) * value;

    return values.empty() ? 0.0 : std::sqrt (sum / static_cast<double> (values.size()));
}
} // namespace

AlignmentMeasurement measureAlignment (FrameSource& original,
                                       const std::array<FrameSource*, 4>& stems,
                                       std::int64_t songFrames,
                                       int sampleRate,
                                       const std::function<bool()>& cancelled,
                                       int maxLag)
{
    AlignmentMeasurement result;
    const auto isCancelled = [&] { return cancelled && cancelled(); };
    const std::vector<FrameSource*> stemList (stems.begin(), stems.end());

    const auto window =
        static_cast<int> (std::min<std::int64_t> (songFrames, static_cast<std::int64_t> (sampleRate) * 5));
    const auto lagRange = std::min (maxLag, window / 4);

    if (window < 64 || lagRange < 1)
        return result;

    // Pick the loudest short probe across the song, then centre the window on it.
    const auto probe = std::min (window, sampleRate / 4);
    const int probes = 16;
    std::vector<float> scratch;
    double loudest = -1.0;
    std::int64_t loudestAt = 0;

    for (int i = 0; i < probes; ++i)
    {
        if (isCancelled())
            return result;

        const auto at = (songFrames - probe) * i / (probes - 1);
        readMono (stemList, at, probe, scratch);
        const auto level = rmsOf (scratch);

        if (level > loudest)
        {
            loudest = level;
            loudestAt = at;
        }
    }

    if (loudest < silenceRms)
        return result;

    result.windowFrames = window;
    result.windowStart = juce::jlimit<std::int64_t> (0, songFrames - window, loudestAt + probe / 2 - window / 2);

    std::vector<float> reference, candidate;
    readMono (stemList, result.windowStart, window, reference);
    readMono ({ &original }, result.windowStart - lagRange, window + 2 * lagRange, candidate);

    if (isCancelled())
        return result;

    // c[k] = sum_n reference[n] * candidate[n + k], k = 0 .. 2 * lagRange, via FFT.
    const auto needed = static_cast<size_t> (window + 2 * lagRange);
    int order = 4;

    while ((size_t { 1 } << order) < needed)
        ++order;

    const auto size = size_t { 1 } << order;
    juce::dsp::FFT fft (order);
    std::vector<std::complex<float>> a (size), b (size), spectrumA (size), spectrumB (size), product (size),
        correlation (size);

    for (size_t i = 0; i < candidate.size(); ++i)
        a[i] = candidate[i];

    for (size_t i = 0; i < reference.size(); ++i)
        b[i] = reference[i];

    fft.perform (a.data(), spectrumA.data(), false);
    fft.perform (b.data(), spectrumB.data(), false);

    for (size_t i = 0; i < size; ++i)
        product[i] = spectrumA[i] * std::conj (spectrumB[i]);

    fft.perform (product.data(), correlation.data(), true);

    // Normalise by the candidate's energy at each lag, so loud passages do not win by size.
    std::vector<double> energy (candidate.size() + 1, 0.0);

    for (size_t i = 0; i < candidate.size(); ++i)
        energy[i + 1] = energy[i] + static_cast<double> (candidate[i]) * candidate[i];

    double referenceEnergy = 0.0;

    for (auto value : reference)
        referenceEnergy += static_cast<double> (value) * value;

    double best = -2.0;
    int bestLag = 0;

    for (int k = 0; k <= 2 * lagRange; ++k)
    {
        const auto candidateEnergy = energy[static_cast<size_t> (k + window)] - energy[static_cast<size_t> (k)];

        if (candidateEnergy <= 0.0 || referenceEnergy <= 0.0)
            continue;

        const auto score = static_cast<double> (correlation[static_cast<size_t> (k)].real())
                         / std::sqrt (candidateEnergy * referenceEnergy);

        if (score > best)
        {
            best = score;
            bestLag = k - lagRange;
        }
    }

    // Recompute the winner in double precision.
    double dot = 0.0;
    double candidateEnergy = 0.0;

    for (int n = 0; n < window; ++n)
    {
        const double c = candidate[static_cast<size_t> (n + bestLag + lagRange)];
        dot += static_cast<double> (reference[static_cast<size_t> (n)]) * c;
        candidateEnergy += c * c;
    }

    result.measurable = candidateEnergy > 0.0 && referenceEnergy > 0.0;
    result.lagFrames = bestLag;
    result.correlation = result.measurable ? dot / std::sqrt (candidateEnergy * referenceEnergy) : 0.0;
    return result;
}

OriginalPlan planOriginal (const juce::File& originalFile,
                           const juce::String& container,
                           juce::AudioFormatManager& formats,
                           std::int64_t songFrames,
                           int sampleRate,
                           const std::array<FrameSource*, 4>& stems,
                           const std::function<bool()>& cancelled)
{
    OriginalPlan plan;
    const bool isMp3 = container.equalsIgnoreCase ("mp3");

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (originalFile));

    if (reader == nullptr)
    {
        plan.reason = "The original file cannot be read.";
        return plan;
    }

    const auto decodedFrames = reader->lengthInSamples;

    if (static_cast<int> (reader->sampleRate) != sampleRate)
    {
        plan.reason = "The original is " + juce::String (reader->sampleRate) + " Hz but the stems are "
                    + juce::String (sampleRate) + " Hz.";
        return plan;
    }

    if (isMp3)
    {
        plan.mp3 = parseMp3 (originalFile);
        plan.leadingSkip = predictedLeadingSkip (plan.mp3);
    }
    else if (decodedFrames != songFrames)
    {
        plan.reason = "The original has " + juce::String (decodedFrames) + " frames but the song has "
                    + juce::String (songFrames) + ".";
        return plan;
    }

    // JUCE counts whole MPEG frames; the worker's decode is the gapless length.
    if (decodedFrames - plan.leadingSkip < songFrames - 2 * std::max (1152, plan.mp3.samplesPerFrame))
    {
        plan.reason = "The original decodes to " + juce::String (decodedFrames - plan.leadingSkip)
                    + " frames after trimming, but the song has " + juce::String (songFrames) + ".";
        return plan;
    }

    auto source = std::make_unique<ReaderFrameSource> (std::move (reader), plan.leadingSkip, songFrames);
    plan.measured = measureAlignment (*source, stems, songFrames, sampleRate, cancelled);

    plan.summary = container.toLowerCase() + " original: leading skip " + juce::String (plan.leadingSkip);

    if (isMp3)
        plan.summary << " ("
                     << (plan.mp3.hasLameTag ? "LAME delay " + juce::String (plan.mp3.encoderDelay) + " + "
                                                   + juce::String (standardDecoderDelay) + " decoder"
                                             : juce::String ("no LAME tag"))
                     << ")";

    if (plan.measured.measurable)
        plan.summary << ", measured lag " << plan.measured.lagFrames << " frames, correlation "
                     << juce::String (plan.measured.correlation, 3);
    else
        plan.summary << ", alignment not measurable (too quiet or cancelled)";

    if (cancelled && cancelled())
    {
        plan.reason = "Checking the original was cancelled.";
        return plan;
    }

    if (! plan.measured.measurable)
    {
        if (isMp3)
        {
            plan.reason = "The MP3 original is too quiet to verify its alignment with the stems.";
            return plan;
        }
    }
    else if (plan.measured.correlation < minimumCorrelation)
    {
        plan.reason = "The original does not match the separated stems (correlation "
                    + juce::String (plan.measured.correlation, 2) + "). It may be a different version of the song.";
        return plan;
    }
    else if (plan.measured.lagFrames != 0)
    {
        plan.reason = "The original is offset from the stems by " + juce::String (plan.measured.lagFrames)
                    + " samples, which its encoder delay does not explain.";
        return plan;
    }

    plan.usable = true;
    plan.source = std::move (source);
    return plan;
}
} // namespace snds::audio
