#include "audio/PlaybackEngine.h"

#include "audio/PrefetchStream.h"
#include "audio/Resampler.h"
#include "audio/SeekTarget.h"
#include "dsp/MasterStage.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace snds::audio
{
namespace
{
constexpr double rampSeconds = 0.010;     // play / pause / seek fades, in device frames
constexpr double wrapFadeSeconds = 0.005; // dip around the loop seam, in source frames
constexpr double abFadeSeconds = 0.030;   // Original / Stem Mix crossfade, in source frames
} // namespace

//==============================================================================
/** Everything tied to one loaded song. After construction the audio thread owns the render
    state; the control thread touches only the atomics, the pending seek and the seek target. */
struct PlaybackEngine::Session
{
    Session (PlaybackSources sources,
             std::int64_t startFrame,
             const PlaybackConfig& configuration,
             double deviceRate,
             bool loop)
        : config (configuration),
          seek (startFrame),
          sourceRate (sources.sampleRate),
          length (sources.lengthFrames),
          prefetch ("SNDS prefetch")
    {
        looping.store (loop);

        const auto ringFrames =
            std::max (2 * config.ioChunkFrames, static_cast<int> (std::ceil (sourceRate * config.readAheadSeconds)));

        for (size_t i = 0; i < stems.size(); ++i)
        {
            stems[i] = std::make_unique<PrefetchStream> (
                std::move (sources.stems[i]), seek, length, ringFrames, config.ioChunkFrames, &looping);
            prefetch.add (*stems[i]);
        }

        if (sources.original != nullptr)
        {
            original = std::make_unique<PrefetchStream> (
                std::move (sources.original), seek, length, ringFrames, config.ioChunkFrames, &looping);
            prefetch.add (*original);

            for (auto& channel : originalBuffer)
                channel.assign (static_cast<size_t> (config.chunkFrames), 0.0f);
        }

        abFade = std::max (2, static_cast<int> (std::lround (sourceRate * abFadeSeconds)));

        safeFrames = std::min (ringFrames / 2, static_cast<int> (std::ceil (sourceRate * config.safeReadAheadSeconds)));
        wrapFade = std::max (2, static_cast<int> (std::lround (sourceRate * wrapFadeSeconds)));
        wrapFadeInProgress = wrapFade;

        for (auto& stem : stemBuffers)
            for (auto& channel : stem)
                channel.assign (static_cast<size_t> (config.chunkFrames), 0.0f);

        for (auto& channel : mixBuffer)
            channel.assign (static_cast<size_t> (config.chunkFrames), 0.0f);

        zeros.assign (static_cast<size_t> (config.chunkFrames), 0.0f);

        generation = seek.load().generation;
        readPosition = startFrame;
        startPosition = startFrame;

        prepareResampler (deviceRate);
    }

    void prepareResampler (double deviceRate)
    {
        resampler.prepare (sourceRate, deviceRate, config.chunkFrames);
        outputLimiter.prepare (deviceRate);
        outputLimiter.setGainDb (0.0f);
        tailPadding = resampler.getLookaheadFrames() + 2;
        rampLength = std::max (2, static_cast<int> (std::lround (deviceRate * rampSeconds)));
        rampIndex = 0;
    }

    void startMixStage (std::unique_ptr<StemMixStage> stage)
    {
        mix = std::move (stage);
        mix->prepare (sourceRate, config.chunkFrames);
    }

    std::size_t reservedBytes() const
    {
        std::size_t bytes = sizeof (float) * 2 * static_cast<std::size_t> (config.chunkFrames) * (stemCount + 2);

        for (const auto& stream : stems)
            bytes += stream->getReservedBytes();

        if (original != nullptr)
            bytes += original->getReservedBytes() + sizeof (float) * 2 * static_cast<std::size_t> (config.chunkFrames);

        return bytes
             + sizeof (float) * 2
                   * static_cast<std::size_t> (resampler.getLookaheadFrames() * 2 + config.chunkFrames * 4)
             + (resampler.isBypass() ? 0 : sizeof (float) * static_cast<std::size_t> (40 * 512 + 2));
    }

    //==============================================================================
    // Seeking.

    /** Control thread, or the audio thread when idle: publishes a requested seek, if any. */
    void flushPendingSeek() noexcept
    {
        const auto frame = pendingSeek.exchange (-1, std::memory_order_acq_rel);

        if (frame >= 0)
        {
            seek.publish (frame);
            prefetch.notify();
        }
    }

    /** Audio thread: adopts a new seek target, if one was published. */
    void adoptSeekTarget() noexcept
    {
        const auto target = seek.load();

        if (target.generation == generation)
            return;

        generation = target.generation;
        readPosition = target.frame;
        startPosition = target.frame;
        pushedReal = 0;
        paddingPushed = 0;
        wrapFadeInProgress = wrapFade;
        abIndex = originalWanted.load (std::memory_order_relaxed) ? abFade : 0; // no crossfade after a seek
        ending = false;
        atEnd = false;
        ready = false;
        rampIndex = 0;
        resampler.reset();
        mix->reset();
        outputLimiter.reset();
    }

    bool hasSafeReadAhead() noexcept
    {
        const auto needed =
            static_cast<int> (std::min<std::int64_t> (safeFrames, std::max<std::int64_t> (0, length - readPosition)));

        for (auto& stream : stems)
            if (stream->available (generation) < needed)
                return false;

        if (original != nullptr && original->available (generation) < needed)
            return false;

        return true;
    }

    //==============================================================================
    enum class Pull
    {
        pulled,
        starved,
        seekRaced
    };

    /** Reads the next chunk from all stems or from none, mixes it, and feeds the resampler. */
    Pull pullChunk() noexcept
    {
        const auto count = static_cast<int> (std::min<std::int64_t> (config.chunkFrames, length - readPosition));

        for (auto& stream : stems)
            if (stream->available (generation) < count)
                return seek.load().generation == generation ? Pull::starved : Pull::seekRaced;

        if (original != nullptr && original->available (generation) < count)
            return seek.load().generation == generation ? Pull::starved : Pull::seekRaced;

        std::array<const float* const*, stemCount> inputs {};

        for (size_t i = 0; i < stems.size(); ++i)
        {
            float* channels[2] = { stemBuffers[i][0].data(), stemBuffers[i][1].data() };
            stemChannelPointers[i][0] = channels[0];
            stemChannelPointers[i][1] = channels[1];
            inputs[i] = stemChannelPointers[i].data();

            if (! stems[i]->read (generation, channels, count))
                return Pull::seekRaced;
        }

        float* mixed[2] = { mixBuffer[0].data(), mixBuffer[1].data() };
        mix->process (inputs, mixed, count);
        applyReference (mixed, count);
        applyLoopSeamDip (mixed, count);
        resampler.push (mixed, count);
        readPosition += count;
        pushedReal += count;
        return Pull::pulled;
    }

    /** Original / Stem Mix: the original is read in lockstep with the stems (so a switch is
        instant and always aligned); this picks or crossfades. The two weights sum to 1 (raised
        cosine), so correlated, aligned signals cannot add up to a louder one. */
    void applyReference (float* const* mixed, int count) noexcept
    {
        if (original == nullptr)
            return;

        float* originalChannels[2] = { originalBuffer[0].data(), originalBuffer[1].data() };

        if (! original->read (generation, originalChannels, count))
            return;

        const int goal = originalWanted.load (std::memory_order_relaxed) ? abFade : 0;

        for (int i = 0; i < count; ++i)
        {
            if (abIndex < goal)
                ++abIndex;
            else if (abIndex > goal)
                --abIndex;

            if (abIndex == 0)
                continue;

            if (abIndex == abFade)
            {
                mixed[0][i] = originalChannels[0][i];
                mixed[1][i] = originalChannels[1][i];
                continue;
            }

            const auto weight = 0.5f
                              * (1.0f
                                 - std::cos (juce::MathConstants<float>::pi * static_cast<float> (abIndex)
                                             / static_cast<float> (abFade)));
            mixed[0][i] += weight * (originalChannels[0][i] - mixed[0][i]);
            mixed[1][i] += weight * (originalChannels[1][i] - mixed[1][i]);
        }
    }

    /** Dips the last and first `wrapFade` frames around the loop seam (when looping), so the
        jump from the end to the start cannot click. */
    void applyLoopSeamDip (float* const* mixed, int count) noexcept
    {
        const bool loopNow = looping.load (std::memory_order_relaxed);
        const float inverse = 1.0f / static_cast<float> (wrapFade);

        for (int i = 0; i < count; ++i)
        {
            float gain = 1.0f;
            const auto toEnd = length - (readPosition + i); // 1 for the last frame

            if (loopNow && toEnd < wrapFade)
                gain = static_cast<float> (toEnd) * inverse;

            const auto sinceWrap = wrapFadeInProgress + i;

            if (sinceWrap + 1 < wrapFade)
                gain *= static_cast<float> (sinceWrap + 1) * inverse;

            if (gain != 1.0f)
            {
                mixed[0][i] *= gain;
                mixed[1][i] *= gain;
            }
        }

        wrapFadeInProgress = std::min (wrapFade, wrapFadeInProgress + count);
    }

    void pushTailPadding() noexcept
    {
        const auto count = static_cast<int> (std::min<std::int64_t> (config.chunkFrames, tailPadding - paddingPushed));
        const float* silence[2] = { zeros.data(), zeros.data() };
        resampler.push (silence, count);
        paddingPushed += count;
    }

    /** Device frames still to come once all real source frames are pushed; unlimited before. */
    std::int64_t remainingOutputFrames() const noexcept
    {
        if (! ending)
            return std::numeric_limits<std::int64_t>::max();

        const auto remaining =
            (static_cast<double> (pushedReal) - resampler.getNextOutputTime()) / resampler.getRatio();
        return std::max<std::int64_t> (0, static_cast<std::int64_t> (std::ceil (remaining - 1.0e-6)));
    }

    /** The transport clock in song frames. */
    std::int64_t currentFrame() const noexcept
    {
        if (atEnd)
            return length;

        const auto raw = static_cast<std::int64_t> (
            std::floor (static_cast<double> (startPosition) + resampler.getNextOutputTime() + 1.0e-6));
        auto frame = raw % length;

        if (frame == 0 && raw > 0 && ! looping.load (std::memory_order_relaxed))
            frame = length;

        return frame;
    }

    bool isSeekInFlight() const noexcept
    {
        return deferredSeek >= 0 || pendingSeek.load (std::memory_order_relaxed) >= 0
            || generation != seek.load().generation;
    }

    float gainAt (int index) const noexcept
    {
        return index >= rampLength ? 1.0f : static_cast<float> (index) / static_cast<float> (rampLength);
    }

    /** The device went away mid-sound: drop the fade, release a waiting seek, and make the next
        start fade in. */
    void silenceAfterDeviceStop() noexcept
    {
        rampIndex = 0;

        if (deferredSeek >= 0)
        {
            seek.publish (deferredSeek);
            deferredSeek = -1;
        }

        flushPendingSeek();
    }

    /** Renders `numFrames` device frames into two (pre-cleared) channels. */
    void render (float* left, float* right, int numFrames, bool wantPlaying) noexcept
    {
        const auto requested = pendingSeek.exchange (-1, std::memory_order_acq_rel);

        if (requested >= 0)
            deferredSeek = requested;

        // A seek waits for the fade-out of what is playing; when silent it goes out at once.
        if (deferredSeek >= 0 && rampIndex == 0)
        {
            seek.publish (deferredSeek);
            deferredSeek = -1;
        }

        const int target = (wantPlaying && deferredSeek < 0) ? rampLength : 0;

        if (rampIndex == 0 && target == 0)
            return;

        // Starting from silence there is nothing to crossfade from: begin on the chosen mix.
        if (rampIndex == 0 && original != nullptr)
            abIndex = originalWanted.load (std::memory_order_relaxed) ? abFade : 0;

        adoptSeekTarget();

        if (! ready)
        {
            if (! hasSafeReadAhead())
                return;

            ready = true;
        }

        // A fade-out consumes exactly its own frames: nothing is read past the point of silence.
        const int limit = target == 0 ? std::min (numFrames, rampIndex) : numFrames;
        int produced = 0;

        while (produced < limit)
        {
            const auto wanted = static_cast<int> (std::min<std::int64_t> (limit - produced, remainingOutputFrames()));

            if (wanted <= 0)
            {
                atEnd = true;
                break;
            }

            float* destination[2] = { left + produced, right + produced };
            const auto count = resampler.pull (destination, wanted);
            produced += count;

            if (count == wanted)
                continue;

            // The resampler needs more source frames.
            if (! ending)
            {
                if (readPosition >= length)
                {
                    if (looping.load (std::memory_order_relaxed))
                    {
                        readPosition = 0;
                        wrapFadeInProgress = 0;
                    }
                    else
                    {
                        ending = true;
                        continue;
                    }
                }

                const auto result = pullChunk();

                if (result == Pull::pulled)
                    continue;

                if (result == Pull::starved)
                {
                    underrunCount.fetch_add (1, std::memory_order_relaxed);
                    ready = false;
                }

                break;
            }

            if (paddingPushed >= tailPadding)
                break; // flushed and still nothing: the song is over

            pushTailPadding();
        }

        // Fade with the transport: in after a start, out before a pause or seek.
        for (int i = 0; i < produced; ++i)
        {
            float gain;

            if (rampIndex < target)
            {
                gain = gainAt (++rampIndex);
            }
            else if (rampIndex > target)
            {
                gain = gainAt (rampIndex--);
            }
            else
            {
                gain = gainAt (rampIndex);
            }

            if (gain != 1.0f)
            {
                left[i] *= gain;
                right[i] *= gain;
            }
        }

        // Interpolation can exceed the source-rate ceiling. Limit the final device samples,
        // including Original and transport fades, without adding latency or changing stereo balance.
        const float* finalInput[2] = { left, right };
        float* finalOutput[2] = { left, right };
        outputLimiter.process (finalInput, finalOutput, produced);

        // Where the audio just stopped dead, the next start must fade in again.
        if (! ready || atEnd)
            rampIndex = 0;

        // The fade-out is complete: a waiting seek goes out now, so a later control-thread seek
        // can never be overtaken by a stale one.
        if (deferredSeek >= 0 && rampIndex == 0)
        {
            seek.publish (deferredSeek);
            deferredSeek = -1;
        }
    }

    //==============================================================================
    const PlaybackConfig config;
    SeekTarget seek;
    const int sourceRate;
    const std::int64_t length;

    std::array<std::unique_ptr<PrefetchStream>, stemCount> stems;
    std::unique_ptr<PrefetchStream> original; // null when there is no usable original
    std::array<std::vector<float>, 2> originalBuffer;
    std::atomic<bool> originalWanted { false };
    int abFade = 2;
    int abIndex = 0; // 0 stem mix .. abFade original
    PrefetchThread prefetch;
    std::unique_ptr<StemMixStage> mix;
    Resampler resampler;
    dsp::MasterStage outputLimiter;
    int safeFrames = 0;
    int tailPadding = 0;
    int wrapFade = 2;
    int rampLength = 2;

    std::array<std::array<std::vector<float>, 2>, stemCount> stemBuffers;
    std::array<const float*, 2> stemChannelPointers[stemCount] {};
    std::array<std::vector<float>, 2> mixBuffer;
    std::vector<float> zeros;

    std::atomic<std::int64_t> pendingSeek { -1 }; // written by the control thread
    std::atomic<bool> looping { false };
    std::atomic<int> underrunCount { 0 };

    // Audio-thread state.
    std::uint32_t generation = 0;
    std::int64_t readPosition = 0;  // next source frame to pull from the streams
    std::int64_t startPosition = 0; // song frame the resampler's time 0 corresponds to
    std::int64_t pushedReal = 0;    // real source frames fed to the resampler since the last seek
    std::int64_t deferredSeek = -1; // a seek waiting for the fade-out
    int paddingPushed = 0;
    int wrapFadeInProgress = 0;
    int rampIndex = 0; // 0 silent .. rampLength full
    bool ending = false;
    bool ready = false;
    bool atEnd = false;
};

//==============================================================================
PlaybackEngine::PlaybackEngine (PlaybackConfig configuration)
    : config (configuration),
      mixStageFactory ([] { return std::make_unique<UnityStemMixStage>(); })
{
}

PlaybackEngine::~PlaybackEngine()
{
    unload();
}

void PlaybackEngine::setMixStageFactory (std::function<std::unique_ptr<StemMixStage>()> factory)
{
    mixStageFactory = std::move (factory);
}

void PlaybackEngine::prepare (double deviceSampleRate, int maxBlockFrames)
{
    jassert (deviceSampleRate > 0.0 && maxBlockFrames > 0);

    for (auto& channel : monoScratch)
        channel.assign (static_cast<size_t> (maxBlockFrames), 0.0f);

    const juce::SpinLock::ScopedLockType lock (sessionLock);
    deviceRate = deviceSampleRate;
    maxBlock = maxBlockFrames;

    if (session != nullptr)
    {
        // A new device rate: rebuild the converter and restart the streams at the playhead.
        const auto position = getPositionFrames();
        session->prepareResampler (deviceRate);
        session->pendingSeek.store (position);
        session->flushPendingSeek();
        buffering.store (true);
        reservedBytes.store (session->reservedBytes(), std::memory_order_relaxed);
    }
}

bool PlaybackEngine::load (PlaybackSources sources, std::int64_t startFrame)
{
    if (deviceRate <= 0.0 || sources.sampleRate <= 0 || sources.lengthFrames <= 0)
        return false;

    for (const auto& stem : sources.stems)
        if (stem == nullptr || stem->getSampleRate() != sources.sampleRate)
            return false;

    if (sources.original != nullptr && sources.original->getSampleRate() != sources.sampleRate)
        return false;

    startFrame = juce::jlimit<std::int64_t> (0, sources.lengthFrames, startFrame);
    auto fresh = std::make_unique<Session> (std::move (sources), startFrame, config, deviceRate, isLooping());
    fresh->startMixStage (mixStageFactory());
    fresh->prefetch.start();

    std::unique_ptr<Session> retired;

    {
        const juce::SpinLock::ScopedLockType lock (sessionLock);
        retired = std::move (session);
        session = std::move (fresh);
        lengthFrames.store (session->length);
        sourceRate.store (session->sourceRate);
        reservedBytes.store (session->reservedBytes() + 2 * sizeof (float) * static_cast<std::size_t> (maxBlock));
        originalPresent.store (session->original != nullptr);
        originalSelected.store (false);
        positionFrames.store (startFrame);
        playing.store (false);
        audible.store (false);
        buffering.store (true);
        reachedEnd.store (false);
        underruns.store (0);
        loaded.store (true, std::memory_order_release);
    }

    retired.reset(); // joins the old prefetch thread outside the lock
    return true;
}

void PlaybackEngine::unload()
{
    std::unique_ptr<Session> retired;

    {
        const juce::SpinLock::ScopedLockType lock (sessionLock);
        retired = std::move (session);
        originalPresent.store (false);
        originalSelected.store (false);
        loaded.store (false, std::memory_order_release);
        playing.store (false);
        audible.store (false);
        lengthFrames.store (0);
        sourceRate.store (0);
        positionFrames.store (0);
        buffering.store (false);
        reachedEnd.store (false);
        reservedBytes.store (0);
    }

    retired.reset();
}

//==============================================================================
void PlaybackEngine::requestSeek (std::int64_t frame)
{
    if (session == nullptr)
        return;

    frame = juce::jlimit<std::int64_t> (0, session->length, frame);
    session->pendingSeek.store (frame, std::memory_order_release);
    positionFrames.store (frame, std::memory_order_release);
    buffering.store (true, std::memory_order_release);
    reachedEnd.store (false, std::memory_order_release);

    // If nothing is sounding the audio thread is not touching the streams: go now, so the
    // read-ahead fills even while paused or with no device running.
    if (! audible.load (std::memory_order_acquire))
        session->flushPendingSeek();
}

void PlaybackEngine::seek (std::int64_t frame)
{
    requestSeek (frame);
}

void PlaybackEngine::play()
{
    if (session == nullptr)
        return;

    if (hasReachedEnd() || (getPositionFrames() >= session->length && ! isLooping()))
        requestSeek (0);

    setPlaying (true);
}

void PlaybackEngine::pause()
{
    setPlaying (false);
}

void PlaybackEngine::stop()
{
    setPlaying (false);
    requestSeek (0);
}

void PlaybackEngine::deviceStopped()
{
    // Holding the session lock keeps a callback that is still winding down out of the render state.
    const juce::SpinLock::ScopedLockType lock (sessionLock);
    playing.store (false, std::memory_order_release);
    audible.store (false, std::memory_order_release);

    if (session != nullptr)
        session->silenceAfterDeviceStop();
}

bool PlaybackEngine::setOriginalSelected (bool shouldPlayOriginal)
{
    if (session == nullptr || session->original == nullptr)
        return false;

    originalSelected.store (shouldPlayOriginal, std::memory_order_release);
    session->originalWanted.store (shouldPlayOriginal, std::memory_order_release);
    return true;
}

void PlaybackEngine::setLooping (bool shouldLoop)
{
    looping.store (shouldLoop, std::memory_order_release);

    if (session != nullptr)
    {
        session->looping.store (shouldLoop, std::memory_order_release);
        session->prefetch.notify();
    }
}

int PlaybackEngine::getReadFailureCount() const noexcept
{
    int total = 0;

    if (session != nullptr)
        for (const auto& stream : session->stems)
            total += stream->getReadFailures();

    return total;
}

void PlaybackEngine::publishStatus (const Session& current) noexcept
{
    audible.store (current.rampIndex > 0, std::memory_order_release);

    // While a seek is on its way the control thread's target position stands.
    if (! current.isSeekInFlight())
    {
        positionFrames.store (current.currentFrame(), std::memory_order_release);
        buffering.store (! current.ready, std::memory_order_release);
        reachedEnd.store (current.atEnd, std::memory_order_release);
    }

    underruns.store (current.underrunCount.load (std::memory_order_relaxed), std::memory_order_relaxed);

    if (current.atEnd)
        playing.store (false, std::memory_order_release);
}

void PlaybackEngine::process (float* const* output, int numChannels, int numFrames) noexcept
{
    for (int channel = 0; channel < numChannels; ++channel)
        std::fill (output[channel], output[channel] + numFrames, 0.0f);

    if (numChannels < 1 || numFrames < 1)
        return;

    const juce::SpinLock::ScopedTryLockType lock (sessionLock);

    if (! lock.isLocked() || session == nullptr)
        return;

    const bool wantPlaying = playing.load (std::memory_order_acquire);

    if (numChannels >= 2)
    {
        session->render (output[0], output[1], numFrames, wantPlaying);
    }
    else
    {
        for (int done = 0; done < numFrames;)
        {
            const auto count = std::min (numFrames - done, maxBlock);
            std::fill_n (monoScratch[0].data(), count, 0.0f);
            std::fill_n (monoScratch[1].data(), count, 0.0f);
            session->render (monoScratch[0].data(), monoScratch[1].data(), count, wantPlaying);

            for (int i = 0; i < count; ++i)
                output[0][done + i] =
                    0.5f * (monoScratch[0][static_cast<size_t> (i)] + monoScratch[1][static_cast<size_t> (i)]);

            done += count;
        }
    }

    publishStatus (*session);
}

bool PlaybackEngine::waitForReadAhead (int deviceFrames, int timeoutMs)
{
    if (session == nullptr)
        return false;

    const auto needed = static_cast<int> (std::ceil (deviceFrames * session->resampler.getRatio()))
                      + session->resampler.getLookaheadFrames() + 3 * config.chunkFrames;
    const auto required = std::max (needed, session->safeFrames);
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;

    for (;;)
    {
        // A seek that went out already, or one the audio thread has yet to publish.
        const auto generation = session->seek.load().generation;
        session->prefetch.notify();
        bool satisfied = true;

        if (session->original != nullptr)
            satisfied = satisfied
                     && (session->original->peekAvailable (generation) >= required
                         || (! isLooping() && session->original->hasReachedLimit (generation)));

        for (const auto& stream : session->stems)
            satisfied = satisfied
                     && (stream->peekAvailable (generation) >= required
                         || (! isLooping() && stream->hasReachedLimit (generation)));

        if (satisfied)
            return true;

        if (juce::Time::getMillisecondCounterHiRes() > deadline)
            return false;

        juce::Thread::sleep (1);
    }
}
} // namespace snds::audio
