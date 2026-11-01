// Audits the audio callback path of PlaybackEngine::process (see "Audio threading contract" in
// docs/architecture.md). The test thread plays the audio thread. Around every process() call a
// thread-local counter watches the global allocator, so any heap allocation or release on the
// callback path fails a test. Reads from the FrameSources and log messages are attributed to the
// thread that made them, which proves file access and logging stay off the callback thread.
//
// This file replaces the global operator new/delete for the whole test executable.

#include <juce_core/juce_core.h>

#include "PlaybackTestKit.h"
#include "app/MixService.h"
#include "audio/PlaybackEngine.h"
#include "dsp/MixProcessor.h"
#include "dsp/ParameterStore.h"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <new>
#include <set>
#include <thread>
#include <vector>

#ifdef _DEBUG
#include <crtdbg.h>
#endif

namespace snds::test::audit
{
thread_local bool tracking = false;
thread_local long long allocationCount = 0;
thread_local long long releaseCount = 0;

/** Counts allocations and releases made by this thread while it is alive. */
class ScopedTracking
{
public:
    ScopedTracking() noexcept
    {
        allocationCount = 0;
        releaseCount = 0;
        tracking = true;
    }

    ~ScopedTracking() { tracking = false; }

    long long allocations() const noexcept { return allocationCount; }
    long long releases() const noexcept { return releaseCount; }
};

inline void noteAllocation() noexcept
{
    if (tracking)
        ++allocationCount;
}

inline void noteRelease() noexcept
{
    if (tracking)
        ++releaseCount;
}

#ifdef _DEBUG
// The debug CRT also reports plain malloc/free, which operator new alone would miss.
int __cdecl crtHook (int allocType, void*, size_t, int, long, const unsigned char*, int)
{
    if (allocType == _HOOK_ALLOC || allocType == _HOOK_REALLOC)
        noteAllocation();
    else if (allocType == _HOOK_FREE)
        noteRelease();

    return 1; // proceed
}
#endif
} // namespace snds::test::audit

//==============================================================================
// Replacement allocation functions: count, then forward to malloc/free.
void* operator new (std::size_t size)
{
    snds::test::audit::noteAllocation();

    if (auto* p = std::malloc (size != 0 ? size : 1))
        return p;

    throw std::bad_alloc();
}

void* operator new[] (std::size_t size)
{
    return operator new (size);
}

void* operator new (std::size_t size, const std::nothrow_t&) noexcept
{
    snds::test::audit::noteAllocation();
    return std::malloc (size != 0 ? size : 1);
}

void* operator new[] (std::size_t size, const std::nothrow_t&) noexcept
{
    return operator new (size, std::nothrow);
}

void* operator new (std::size_t size, std::align_val_t alignment)
{
    snds::test::audit::noteAllocation();

    if (auto* p = _aligned_malloc (size != 0 ? size : 1, static_cast<std::size_t> (alignment)))
        return p;

    throw std::bad_alloc();
}

void* operator new[] (std::size_t size, std::align_val_t alignment)
{
    return operator new (size, alignment);
}

void operator delete (void* p) noexcept
{
    snds::test::audit::noteRelease();
    std::free (p);
}

void operator delete[] (void* p) noexcept
{
    operator delete (p);
}
void operator delete (void* p, std::size_t) noexcept
{
    operator delete (p);
}
void operator delete[] (void* p, std::size_t) noexcept
{
    operator delete (p);
}
void operator delete (void* p, const std::nothrow_t&) noexcept
{
    operator delete (p);
}
void operator delete[] (void* p, const std::nothrow_t&) noexcept
{
    operator delete (p);
}

void operator delete (void* p, std::align_val_t) noexcept
{
    snds::test::audit::noteRelease();
    _aligned_free (p);
}

void operator delete[] (void* p, std::align_val_t alignment) noexcept
{
    operator delete (p, alignment);
}
void operator delete (void* p, std::size_t, std::align_val_t alignment) noexcept
{
    operator delete (p, alignment);
}
void operator delete[] (void* p, std::size_t, std::align_val_t alignment) noexcept
{
    operator delete (p, alignment);
}

//==============================================================================
namespace snds
{
namespace
{
using namespace snds::test;
using namespace snds::audio;

/** A synthetic source that remembers which threads read it. */
class WatchedSource final : public FrameSource
{
public:
    WatchedSource (std::unique_ptr<FrameSource> inner,
                   std::shared_ptr<std::set<std::thread::id>> threads,
                   std::shared_ptr<std::mutex> lock)
        : wrapped (std::move (inner)),
          readers (std::move (threads)),
          mutex (std::move (lock))
    {
    }

    int getSampleRate() const noexcept override { return wrapped->getSampleRate(); }
    std::int64_t getLengthInFrames() const noexcept override { return wrapped->getLengthInFrames(); }

    bool read (std::int64_t startFrame, float* const* destination, int numFrames) override
    {
        {
            const std::lock_guard<std::mutex> guard (*mutex);
            readers->insert (std::this_thread::get_id());
        }

        return wrapped->read (startFrame, destination, numFrames);
    }

private:
    std::unique_ptr<FrameSource> wrapped;
    std::shared_ptr<std::set<std::thread::id>> readers;
    std::shared_ptr<std::mutex> mutex;
};

/** Records which thread logged. */
class RecordingLogger final : public juce::Logger
{
public:
    void logMessage (const juce::String&) override
    {
        const std::lock_guard<std::mutex> guard (mutex);
        threads.insert (std::this_thread::get_id());
    }

    bool loggedFrom (std::thread::id id)
    {
        const std::lock_guard<std::mutex> guard (mutex);
        return threads.count (id) > 0;
    }

private:
    std::mutex mutex;
    std::set<std::thread::id> threads;
};

struct Totals
{
    long long allocations = 0;
    long long releases = 0;
    int blocks = 0;
    float peak = 0.0f;
};

/** The pieces of one audited run. */
struct Harness
{
    explicit Harness (int rate = 44100, std::int64_t frames = 44100 * 4, int readDelayMs = 0)
    {
        sources.sampleRate = rate;
        sources.lengthFrames = frames;

        for (int stem = 0; stem < stemCount; ++stem)
            sources.stems[static_cast<size_t> (stem)] = watch (std::make_unique<SyntheticSource> (
                rate,
                frames,
                [stem] (std::int64_t f, int c) { return stemMarker (stem, f, c) - 0.25f; },
                stem == 3 ? readDelayMs : 0));
    }

    void addOriginal()
    {
        sources.original = watch (std::make_unique<SyntheticSource> (
            sources.sampleRate,
            sources.lengthFrames,
            [] (std::int64_t f, int c) { return 0.5f * (expectedUnitySum (f, c) - 1.0f); }));
    }

    std::unique_ptr<FrameSource> watch (std::unique_ptr<FrameSource> inner)
    {
        return std::make_unique<WatchedSource> (std::move (inner), readerThreads, readerMutex);
    }

    /** Calls process() `blocks` times, with allocation tracking around each call only. */
    Totals run (PlaybackEngine& engine, int blocks, int blockSize, int channels = 2, bool waitForData = true)
    {
        Totals totals;
        left.assign (static_cast<size_t> (blockSize), 0.0f);
        right.assign (static_cast<size_t> (blockSize), 0.0f);
        float* pointers[2] = { left.data(), right.data() };

        for (int i = 0; i < blocks; ++i)
        {
            if (waitForData)
                engine.waitForReadAhead (blockSize, 20000);

            long long allocations = 0;
            long long releases = 0;

            {
                audit::ScopedTracking tracking;
                engine.process (pointers, channels, blockSize);
                allocations = tracking.allocations();
                releases = tracking.releases();
            }

            totals.allocations += allocations;
            totals.releases += releases;
            ++totals.blocks;

            for (int k = 0; k < blockSize; ++k)
                totals.peak = std::max ({ totals.peak,
                                          std::abs (left[static_cast<size_t> (k)]),
                                          std::abs (right[static_cast<size_t> (k)]) });
        }

        return totals;
    }

    bool readFromThread (std::thread::id id)
    {
        const std::lock_guard<std::mutex> guard (*readerMutex);
        return readerThreads->count (id) > 0;
    }

    PlaybackSources sources;
    std::shared_ptr<std::set<std::thread::id>> readerThreads = std::make_shared<std::set<std::thread::id>>();
    std::shared_ptr<std::mutex> readerMutex = std::make_shared<std::mutex>();
    std::vector<float> left, right;
};

class AudioCallbackAuditTests final : public juce::UnitTest
{
public:
    AudioCallbackAuditTests()
        : juce::UnitTest ("AuditCallback", "snds")
    {
    }

    void runTest() override
    {
        beginTest ("Setup");
#ifdef _DEBUG
        _CrtSetAllocHook (audit::crtHook);
#endif
        testTheAuditSeesAllocations();
        testStemsAtMatchingRate();
        testResamplingPaths();
        testOriginalAB();
        testLoopAndEnd();
        testTransportChurn();
        testUnderrun();
        testMixProcessor();
        testIdleStates();
        testNoIoOrLoggingOnTheCallbackThread();
#ifdef _DEBUG
        _CrtSetAllocHook (nullptr);
#endif
    }

private:
    void expectClean (const Totals& totals, const juce::String& what, bool expectSound = true)
    {
        expectEquals (totals.allocations, 0LL, what + ": heap allocations inside process()");
        expectEquals (totals.releases, 0LL, what + ": heap releases inside process()");
        expect (totals.blocks > 0, what + ": ran");

        if (expectSound)
            expect (totals.peak > 0.0f, what + ": the path produced sound, so it really ran");
    }

    void testTheAuditSeesAllocations()
    {
        beginTest ("AuditCallback: the allocation counter sees new, containers and (debug) malloc");
        {
            audit::ScopedTracking tracking;
            auto* p = new int (3);
            delete p;
            expect (tracking.allocations() >= 1 && tracking.releases() >= 1, "new/delete");
        }
        {
            audit::ScopedTracking tracking;
            std::vector<float> v (100, 1.0f);
            expect (tracking.allocations() >= 1, "std::vector");
        }
        {
            audit::ScopedTracking tracking;
            juce::String s ("a string long enough to need the heap, not just a small buffer ...");
            expect (tracking.allocations() >= 1, "juce::String");
        }
#ifdef _DEBUG
        {
            audit::ScopedTracking tracking;
            void* raw = std::malloc (64);
            std::free (raw);
            expect (tracking.allocations() >= 1, "malloc (debug CRT hook)");
        }
#endif
        {
            // Allocation on another thread is not charged to this one (the thread is created first).
            std::atomic<bool> go { false };
            std::thread other (
                [&go]
                {
                    while (! go.load())
                        std::this_thread::yield();

                    delete new int (1);
                });

            audit::ScopedTracking tracking;
            go = true;
            other.join();
            expectEquals (tracking.allocations(), 0LL);
        }
    }

    void testStemsAtMatchingRate()
    {
        beginTest ("AuditCallback: stems at the device rate, stereo and mono output, any block size");

        for (const int block : { 1, 64, 480, 1024, 4096 })
        {
            Harness harness;
            PlaybackEngine engine;
            engine.prepare (44100.0, 4096);
            expect (engine.load (std::move (harness.sources)));
            engine.play();
            expectClean (harness.run (engine, block == 1 ? 3000 : 40, block), "block " + juce::String (block));
        }

        Harness harness;
        PlaybackEngine engine;
        engine.prepare (44100.0, 512);
        expect (engine.load (std::move (harness.sources)));
        engine.play();
        expectClean (harness.run (engine, 30, 256, 1), "mono output");
    }

    void testResamplingPaths()
    {
        beginTest ("AuditCallback: every resampling ratio, up and down");

        for (const auto& [source, device] : { std::pair { 44100, 48000.0 },
                                              std::pair { 48000, 44100.0 },
                                              std::pair { 96000, 48000.0 },
                                              std::pair { 48000, 96000.0 },
                                              std::pair { 192000, 48000.0 } })
        {
            Harness harness (source, source / 2);
            PlaybackEngine engine;
            engine.prepare (device, 512);
            expect (engine.load (std::move (harness.sources)));
            engine.play();
            expectClean (harness.run (engine, source >= 96000 ? 8 : 20, 512),
                         juce::String (source) + " -> " + juce::String (device));
        }
    }

    void testOriginalAB()
    {
        beginTest ("AuditCallback: Original/Stem Mix switching and the crossfade, resampled");
        Harness harness (44100, 44100 * 3);
        harness.addOriginal();
        PlaybackEngine engine;
        engine.prepare (48000.0, 512);
        expect (engine.load (std::move (harness.sources)));
        engine.play();

        Totals total;

        for (int round = 0; round < 16; ++round)
        {
            engine.setOriginalSelected (round % 2 == 0);
            const auto part = harness.run (engine, 5, 512);
            total.allocations += part.allocations;
            total.releases += part.releases;
            total.blocks += part.blocks;
            total.peak = std::max (total.peak, part.peak);
        }

        expectClean (total, "A/B toggling");
    }

    void testLoopAndEnd()
    {
        beginTest ("AuditCallback: loop wraps, loop off, the tail flush and the end of the song");
        Harness harness (44100, 9000);
        harness.addOriginal();
        PlaybackEngine engine;
        engine.prepare (48000.0, 512);
        expect (engine.load (std::move (harness.sources)));
        engine.setOriginalSelected (true);
        engine.setLooping (true);
        engine.play();

        auto total = harness.run (engine, 60, 512); // several laps
        engine.setLooping (false);
        const auto rest = harness.run (engine, 40, 512); // runs off the end and keeps being called after it
        total.allocations += rest.allocations;
        total.releases += rest.releases;
        total.blocks += rest.blocks;
        expectClean (total, "loop, then end");
        expect (engine.hasReachedEnd());
    }

    void testTransportChurn()
    {
        beginTest ("AuditCallback: seeks, pauses, stops and loop toggles while playing");
        Harness harness (48000, 48000 * 6);
        harness.addOriginal();
        PlaybackEngine engine;
        engine.prepare (44100.0, 512);
        expect (engine.load (std::move (harness.sources)));
        engine.play();

        juce::Random random (99);
        Totals total;

        for (int step = 0; step < 150; ++step)
        {
            switch (random.nextInt (8))
            {
                case 0:
                    engine.seek (random.nextInt (48000 * 6));
                    break;
                case 1:
                    engine.seek (48000 * 6 - random.nextInt (500));
                    break;
                case 2:
                    engine.pause();
                    break;
                case 3:
                    engine.play();
                    break;
                case 4:
                    engine.setLooping (random.nextBool());
                    break;
                case 5:
                    engine.setOriginalSelected (random.nextBool());
                    break;
                case 6:
                    engine.stop();
                    break;
                default:
                    break;
            }

            const auto part = harness.run (engine, 1 + random.nextInt (3), 64 + random.nextInt (900));
            total.allocations += part.allocations;
            total.releases += part.releases;
            total.blocks += part.blocks;
            total.peak = std::max (total.peak, part.peak);
        }

        expectClean (total, "transport churn");
    }

    void testUnderrun()
    {
        beginTest ("AuditCallback: starvation, silence and re-buffering");
        Harness harness (44100, 44100 * 3, 3); // one stem reads slowly
        PlaybackEngine engine;
        engine.prepare (44100.0, 512);
        expect (engine.load (std::move (harness.sources)));
        engine.play();
        engine.waitForReadAhead (512, 20000);
        const auto total = harness.run (engine, 4000, 512, 2, false); // never waits: starves
        expect (engine.getUnderrunCount() > 0, "the run really starved");
        expectClean (total, "underrun path", false);
    }

    void testMixProcessor()
    {
        beginTest ("AuditCallback: the Phase 4 MixProcessor in the stem mix seam, with live parameter changes");
        MixService service; // the production adapter around MixProcessor
        auto& parameters = service.parameters();
        Harness harness (44100, 44100 * 3);
        PlaybackEngine engine;
        engine.setMixStageFactory (service.stageFactory());
        engine.prepare (48000.0, 512);
        expect (engine.load (std::move (harness.sources)));
        engine.play();

        parameters.set (dsp::GlobalControl::reverbReturn, 0.5f);
        parameters.set (dsp::GlobalControl::delayReturn, 0.5f);
        juce::Random random (5);
        Totals total;

        for (int round = 0; round < 40; ++round)
        {
            // The message thread's side: parameter writes (allocation here is fine), then audio blocks.
            const auto stem = static_cast<dsp::Stem> (random.nextInt (dsp::numStems));
            parameters.set (stem, dsp::StemControl::gain, random.nextFloat() * 12.0f - 12.0f);
            parameters.set (stem, dsp::StemControl::pan, random.nextFloat() * 2.0f - 1.0f);
            parameters.set (stem, dsp::StemControl::reverbSend, random.nextFloat());
            parameters.set (stem, dsp::StemControl::delaySend, random.nextFloat());
            parameters.set (stem, dsp::StemControl::lowCut, random.nextBool() ? 20.0f : 400.0f);
            parameters.set (stem, dsp::StemControl::mute, random.nextInt (5) == 0 ? 1.0f : 0.0f);
            parameters.set (stem, dsp::StemControl::solo, random.nextInt (7) == 0 ? 1.0f : 0.0f);

            if (round % 9 == 0)
                engine.seek (random.nextInt (44100 * 3));

            const auto part = harness.run (engine, 3, 512);
            total.allocations += part.allocations;
            total.releases += part.releases;
            total.blocks += part.blocks;
            total.peak = std::max (total.peak, part.peak);
        }

        expectClean (total, "MixProcessor");
    }

    void testIdleStates()
    {
        beginTest ("AuditCallback: nothing loaded, not playing, and after a device stop");
        PlaybackEngine engine;
        Harness harness;
        expectClean (harness.run (engine, 10, 512, 2, false), "no session", false);

        engine.prepare (44100.0, 512);
        expect (engine.load (std::move (harness.sources)));
        expectClean (harness.run (engine, 10, 512, 2, false), "paused", false);

        engine.play();
        harness.run (engine, 5, 512);
        engine.deviceStopped();
        expectClean (harness.run (engine, 10, 512, 2, false), "after deviceStopped", false);

        engine.unload();
        expectClean (harness.run (engine, 10, 512, 2, false), "after unload", false);
    }

    void testNoIoOrLoggingOnTheCallbackThread()
    {
        beginTest ("AuditCallback: sources are read only by the prefetch thread; the callback thread never logs");
        RecordingLogger logger;
        juce::Logger::setCurrentLogger (&logger);

        Harness harness (44100, 44100 * 2);
        harness.addOriginal();
        PlaybackEngine engine;
        engine.prepare (48000.0, 512);
        expect (engine.load (std::move (harness.sources)));
        engine.play();

        for (int round = 0; round < 6; ++round)
        {
            engine.setOriginalSelected (round % 2 == 0);
            engine.seek (round * 9000);
            harness.run (engine, 12, 512);
        }

        engine.setLooping (true);
        harness.run (engine, 30, 512);

        juce::Logger::setCurrentLogger (nullptr);
        const auto me = std::this_thread::get_id();
        expect (! harness.readFromThread (me), "no FrameSource::read on the audio thread");
        expect (! harness.readerThreads->empty(), "the sources were read (by the prefetch thread)");
        expect (! logger.loggedFrom (me), "no log message from the audio thread");
    }
};

AudioCallbackAuditTests audioCallbackAuditTests;
} // namespace
} // namespace snds
