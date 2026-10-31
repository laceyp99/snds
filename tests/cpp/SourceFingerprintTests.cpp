#include <juce_core/juce_core.h>

#include "TestHelpers.h"
#include "model/SourceFingerprint.h"

#include <deque>
#include <mutex>

namespace snds
{
namespace
{
using Status = FingerprintResult::Status;

juce::String hashOf (const juce::String& text)
{
    const auto utf8 = text.toStdString();
    return Sha256::hash (utf8.data(), utf8.size());
}

juce::String wholeFileHash (const juce::File& file)
{
    juce::MemoryBlock all;
    file.loadFileAsData (all);
    return Sha256::hash (all.getData(), all.getSize());
}

juce::File writeRandomFile (const juce::File& file, juce::int64 size, int seed)
{
    juce::Random random (seed);
    juce::FileOutputStream out (file);
    out.setPosition (0);
    out.truncate();

    juce::MemoryBlock block (64 * 1024);

    for (juce::int64 written = 0; written < size;)
    {
        for (size_t i = 0; i < block.getSize(); ++i)
            block[i] = static_cast<char> (random.nextInt (256));

        const auto part =
            static_cast<size_t> (std::min<juce::int64> (size - written, static_cast<juce::int64> (block.getSize())));
        out.write (block.getData(), part);
        written += static_cast<juce::int64> (part);
    }

    return file;
}

class SourceFingerprintTests final : public juce::UnitTest
{
public:
    SourceFingerprintTests()
        : juce::UnitTest ("Source fingerprint", "snds")
    {
    }

    void runTest() override
    {
        runKnownVectors();
        runFileTests();
        runAsyncTests();
    }

private:
    void runKnownVectors()
    {
        beginTest ("SHA-256 matches the FIPS 180-2 test vectors");
        expectEquals (hashOf ({}), juce::String ("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
        expectEquals (hashOf ("abc"),
                      juce::String ("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        expectEquals (hashOf ("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
                      juce::String ("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

        const std::string millionA (1000000, 'a');
        expectEquals (Sha256::hash (millionA.data(), millionA.size()),
                      juce::String ("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));

        beginTest ("Chunking does not change the digest, and a finished hash is spent");
        {
            const std::string text = "The quick brown fox jumps over the lazy dog";
            Sha256 pieces;

            for (const char c : text)
                pieces.update (&c, 1);

            expectEquals (pieces.finish(),
                          juce::String ("d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"));
            expect (pieces.finish().isEmpty());
        }

        beginTest ("The cache key is <sha256>-<size>");
        {
            SourceFingerprint fingerprint;
            fingerprint.sha256 = hashOf ("abc");
            fingerprint.sizeBytes = 3;
            expectEquals (fingerprint.cacheKey(), fingerprint.sha256 + "-3");
        }
    }

    void runFileTests()
    {
        const test::ScopedTempDirectory temp;

        beginTest ("A multi-chunk file hashes like a single in-memory hash, with ordered progress");
        {
            const auto file = writeRandomFile (temp.child ("big.bin"), 3 * 1024 * 1024 + 12345, 7);
            std::vector<std::pair<juce::int64, juce::int64>> reports;
            const auto result =
                computeFingerprint (file,
                                    [&] (juce::int64 done, juce::int64 total)
                                    {
                                        reports.emplace_back (done, total);
                                        juce::Thread::sleep (60); // make the 50 ms throttle report every chunk
                                    });

            expect (result.ok(), result.error);
            expect (result.fingerprint.sizeBytes == file.getSize());
            expectEquals (result.fingerprint.sha256, wholeFileHash (file));
            expectEquals (result.fingerprint.cacheKey(),
                          result.fingerprint.sha256 + "-" + juce::String (file.getSize()));

            expect (! reports.empty());
            expect (reports.back().first == file.getSize() && reports.back().second == file.getSize());

            for (size_t i = 1; i < reports.size(); ++i)
                expect (reports[i].first > reports[i - 1].first, "progress only moves forward");
        }

        beginTest ("An empty file is hashed (the empty digest) with size 0");
        {
            const auto empty = temp.child ("empty.bin");
            expect (empty.create().wasOk());
            const auto result = computeFingerprint (empty);
            expect (result.ok(), result.error);
            expect (result.fingerprint.sizeBytes == 0);
            expectEquals (result.fingerprint.sha256, hashOf ({}));
        }

        beginTest ("Cancelling stops the hash without a result");
        {
            const auto file = writeRandomFile (temp.child ("cancel.bin"), 5 * 1024 * 1024, 9);
            int polls = 0;
            const auto result = computeFingerprint (file, {}, [&] { return ++polls > 2; });
            expect (result.status == Status::cancelled);
            expect (result.error.isEmpty());
            expectEquals (polls, 3);

            expect (computeFingerprint (file, {}, [] { return true; }).status == Status::cancelled);
        }

        beginTest ("A missing or unreadable file is reported with advice");
        {
            const auto missing = computeFingerprint (temp.child ("missing.wav"));
            expect (missing.status == Status::unreadable);
            expect (missing.error.contains ("missing.wav") && missing.error.contains ("locked"), missing.error);
            expect (computeFingerprint (temp.get()).status == Status::unreadable, "a folder is not a file");
        }

        beginTest ("A file that grows while it is read is reported as changed");
        {
            const auto file = writeRandomFile (temp.child ("growing.bin"), 4 * 1024 * 1024, 11);
            bool appended = false;
            const auto result = computeFingerprint (
                file,
                [&] (juce::int64, juce::int64)
                {
                    if (! appended)
                    {
                        appended = true;
                        file.appendText ("more");
                    }
                },
                []
                {
                    juce::Thread::sleep (60);
                    return false;
                });

            expect (appended);
            expect (result.status == Status::changedWhileReading, result.error);
            expect (result.error.contains ("changed while"));
        }
    }

    //==========================================================================
    struct Queue
    {
        FingerprintJob::Dispatcher dispatcher()
        {
            return [this] (std::function<void()> work)
            {
                const std::lock_guard<std::mutex> lock (mutex);
                items.push_back (std::move (work));
            };
        }

        template <typename Predicate>
        bool pumpUntil (Predicate&& done, int timeoutMs)
        {
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;

            for (;;)
            {
                for (;;)
                {
                    std::function<void()> work;

                    {
                        const std::lock_guard<std::mutex> lock (mutex);

                        if (items.empty())
                            break;

                        work = std::move (items.front());
                        items.pop_front();
                    }

                    work();
                }

                if (done())
                    return true;

                if (juce::Time::getMillisecondCounterHiRes() > deadline)
                    return false;

                juce::Thread::sleep (5);
            }
        }

        std::mutex mutex;
        std::deque<std::function<void()>> items;
    };

    void runAsyncTests()
    {
        const test::ScopedTempDirectory temp;
        const auto file = writeRandomFile (temp.child ("async.bin"), 4 * 1024 * 1024, 21);

        beginTest ("The job delivers progress and the result in order on the dispatcher");
        {
            Queue queue;
            std::vector<juce::int64> done;
            std::optional<FingerprintResult> result;
            FingerprintJob job (
                file,
                [&] (juce::int64 bytes, juce::int64 total)
                {
                    expect (! result.has_value(), "no progress after the result");
                    expect (total == file.getSize());
                    done.push_back (bytes);
                },
                [&] (const FingerprintResult& r) { result = r; },
                queue.dispatcher());

            expect (queue.pumpUntil ([&] { return result.has_value(); }, 30000));
            expect (result && result->ok());
            expect (result && result->fingerprint.sha256 == wholeFileHash (file));
            expect (! done.empty() && done.back() == file.getSize());
        }

        beginTest ("Cancelling a job reports cancelled");
        {
            const auto bigger = writeRandomFile (temp.child ("bigger.bin"), 48 * 1024 * 1024, 5);
            Queue queue;
            std::optional<FingerprintResult> result;
            FingerprintJob job (bigger, {}, [&] (const FingerprintResult& r) { result = r; }, queue.dispatcher());
            job.cancel();
            expect (queue.pumpUntil ([&] { return result.has_value(); }, 30000));
            expect (result && (result->status == Status::cancelled || result->ok()), "cancelled (or already done)");
        }

        beginTest ("Destroying a running job stops it quickly and delivers nothing afterwards");
        {
            const auto bigger = temp.child ("bigger.bin");
            Queue queue;
            bool delivered = false;
            const auto before = juce::Time::getMillisecondCounterHiRes();

            {
                FingerprintJob job (
                    bigger,
                    [&] (juce::int64, juce::int64) { delivered = true; },
                    [&] (const FingerprintResult&) { delivered = true; },
                    queue.dispatcher());
            }

            expect (juce::Time::getMillisecondCounterHiRes() - before < 10000.0);
            queue.pumpUntil ([] { return false; }, 50);
            expect (! delivered);
        }
    }
};

SourceFingerprintTests sourceFingerprintTests;
} // namespace
} // namespace snds
