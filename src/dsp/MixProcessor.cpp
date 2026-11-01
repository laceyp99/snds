#include "dsp/MixProcessor.h"

#include <algorithm>

namespace snds::dsp
{
MixProcessor::MixProcessor (ParameterStore& store)
    : parameters (store)
{
    prepare (48000.0, 256);
}

void MixProcessor::prepare (double sourceRate, int maxBlockFrames)
{
    rate = sourceRate;
    maxFrames = std::max (1, maxBlockFrames);

    const auto size = static_cast<std::size_t> (maxFrames);
    storage.assign (size * 12, 0.0f);

    auto* next = storage.data();
    for (auto& channels : stemChannels)
        for (auto& channel : channels)
        {
            channel = next;
            next += size;
        }

    for (auto* channels : { &reverbReturn, &delayReturn })
        for (auto& channel : *channels)
        {
            channel = next;
            next += size;
        }

    for (auto& filter : filters)
        filter.prepare (rate);
    for (auto& stage : levelStages)
        stage.prepare (rate);

    sends.prepare (rate, maxFrames);
    reverb.prepare (rate);
    delay.prepare (rate);
    master.prepare (rate);

    for (auto& meter : stemMeters)
        meter.prepare (rate);
    masterMeter.prepare (rate);
    limiterInputMeter.prepare (rate);
    limiterReductionMeter.prepare (rate);

    visualAnalyzer.prepare (rate);

    reset();
}

void MixProcessor::applyParameters() noexcept
{
    const auto audible = stemAudibility (snapshot);

    for (int i = 0; i < numStems; ++i)
    {
        const auto stem = static_cast<Stem> (i);
        const auto s = static_cast<std::size_t> (i);

        levelStages[s].setTargets (
            snapshot.get (stem, StemControl::gain), audible[s], snapshot.get (stem, StemControl::pan));
        filters[s].setTargets (snapshot.get (stem, StemControl::lowCut), snapshot.get (stem, StemControl::highCut));
    }

    sends.setTargets (snapshot);
    reverb.setReturn (snapshot.get (GlobalControl::reverbReturn));
    delay.setTempo (static_cast<double> (snapshot.get (GlobalControl::tempoBpm)),
                    toDelayDivision (snapshot.get (GlobalControl::delayDivision)));
    delay.setFeedback (snapshot.get (GlobalControl::delayFeedback));
    delay.setReturn (snapshot.get (GlobalControl::delayReturn));
    master.setGainDb (snapshot.get (GlobalControl::masterGain));
}

void MixProcessor::reset() noexcept
{
    // Pick up whatever the controls say right now, then jump to it without a ramp.
    ParameterSnapshot fresh;
    if (parameters.read (fresh) != ParameterStore::ReadResult::contended)
        snapshot = fresh;

    applyParameters();

    for (auto& stage : levelStages)
        stage.snapToTargets();
    for (auto& filter : filters)
        filter.reset();

    sends.reset();
    reverb.reset();
    delay.reset();
    master.reset();

    for (auto& meter : stemMeters)
        meter.reset();
    masterMeter.reset();
    limiterInputMeter.reset();
    limiterReductionMeter.reset();
    meterStore.publish (MeterSnapshot());
    visualAnalyzer.reset();

    lastFrames = 0;
}

std::array<const float* const*, numStems> MixProcessor::lastStemSignals() const noexcept
{
    std::array<const float* const*, numStems> signals {};

    for (std::size_t i = 0; i < signals.size(); ++i)
        signals[i] = stemChannels[i].data();

    return signals;
}

void MixProcessor::process (const std::array<const float* const*, numStems>& stems,
                            float* const* output,
                            int numFrames) noexcept
{
    const juce::ScopedNoDenormals noDenormals;

    for (int offset = 0; offset < numFrames; offset += maxFrames)
        processChunk (stems, output, offset, std::min (maxFrames, numFrames - offset));
}

void MixProcessor::processChunk (const std::array<const float* const*, numStems>& stems,
                                 float* const* output,
                                 int offset,
                                 int numFrames) noexcept
{
    if (parameters.read (snapshot) == ParameterStore::ReadResult::updated)
        applyParameters();

    // 1. Filters, then fader/mute/solo/pan, into the per-stem buffers (which are the Phase 5 tap).
    for (std::size_t i = 0; i < stems.size(); ++i)
    {
        const std::array<const float*, 2> in { stems[i][0] + offset, stems[i][1] + offset };
        filters[i].process (in.data(), stemChannels[i].data(), numFrames);
        levelStages[i].process (stemChannels[i].data(), stemChannels[i].data(), numFrames);
        stemMeters[i].update (stemChannels[i].data(), numFrames);
    }

    // 2. Sends from the post-pan signals, into the shared effects.
    sends.beginBlock (numFrames);
    for (int i = 0; i < numStems; ++i)
        sends.addStem (i, stemChannels[static_cast<std::size_t> (i)].data(), numFrames);

    reverb.process (sends.reverbBus(), reverbReturn.data(), numFrames);
    delay.process (sends.delayBus(), delayReturn.data(), numFrames);

    // 3. Sum in float, in a fixed order, then the master stage.
    const std::array<float*, 2> out { output[0] + offset, output[1] + offset };

    for (std::size_t c = 0; c < 2; ++c)
    {
        const float* s0 = stemChannels[0][c];
        const float* s1 = stemChannels[1][c];
        const float* s2 = stemChannels[2][c];
        const float* s3 = stemChannels[3][c];
        const float* rv = reverbReturn[c];
        const float* dl = delayReturn[c];
        float* o = out[c];

        for (int i = 0; i < numFrames; ++i)
            o[i] = (((s0[i] + s1[i]) + s2[i]) + s3[i] + rv[i]) + dl[i];
    }

    const auto info = master.process (out.data(), out.data(), numFrames);

    // 4. Meters.
    masterMeter.update (out.data(), numFrames);
    limiterInputMeter.updatePeak (info.inputPeak, numFrames);
    limiterReductionMeter.updatePeak (reductionToDecibels (info.minGain), numFrames);

    MeterSnapshot meters;
    for (std::size_t i = 0; i < stemMeters.size(); ++i)
        meters.stems[i] = { stemMeters[i].peak(), stemMeters[i].rms() };
    meters.masterPeak = masterMeter.peak();
    meters.masterRms = masterMeter.rms();
    meters.limiterInputPeak = limiterInputMeter.peak();
    meters.limiterReductionDb = limiterReductionMeter.peak();
    meterStore.publish (meters);

    // 5. Visual analysis of the same post-pan signals (read-only; the mix never depends on it).
    std::array<const float* const*, numStems> tapped {};
    std::array<float, numStems> pans {};
    for (std::size_t i = 0; i < tapped.size(); ++i)
    {
        tapped[i] = stemChannels[i].data();
        pans[i] = levelStages[i].getPanPosition();
    }
    visualAnalyzer.process (tapped, pans, snapshot.get (GlobalControl::tempoBpm), numFrames);

    lastFrames = numFrames;
}
} // namespace snds::dsp
