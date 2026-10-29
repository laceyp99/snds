#include "TestUtilities.h"
#include "app/SourceFingerprint.h"

#include <thread>

namespace snds::test
{

namespace
{
juce::File writeRandomFile (const juce::File& file, juce::int64 numBytes, juce::int64 seed)
{
    file.getParentDirectory().createDirectory();
    juce::FileOutputStream out (file);
    out.setPosition (0);
    out.truncate();

    juce::Random random (seed);
    juce::HeapBlock<char> block (1024 * 1024);

    for (juce::int64 written = 0; written < numBytes;)
    {
        const auto chunk = static_cast<int> (juce::jmin<juce::int64> (1024 * 1024, numBytes - written));
        random.fillBitsRandomly (block.getData(), static_cast<size_t> (chunk));
        out.write (block.getData(), static_cast<size_t> (chunk));
        written += chunk;
    }

    return file;
}

constexpr HashBackend allBackends[] = { HashBackend::windowsCng, HashBackend::juceSha256 };
} // namespace

class SourceFingerprintTests final : public juce::UnitTest
{
public:
    SourceFingerprintTests()
        : juce::UnitTest ("Source fingerprint", testCategory)
    {
    }

    void runTest() override
    {
        const ScopedTempDirectory temp;

        beginTest ("Known SHA-256 answers on every backend");
        {
            for (const auto backend : allBackends)
            {
                const auto backendName = juce::String (toString (backend));
                expectEquals (sha256Hex ("", 0, backend),
                              juce::String ("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
                              backendName);
                expectEquals (sha256Hex ("abc", 3, backend),
                              juce::String ("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
                              backendName);
                const char* twoBlocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
                expectEquals (sha256Hex (twoBlocks, std::strlen (twoBlocks), backend),
                              juce::String ("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
                              backendName);
            }
        }

        beginTest ("File hashes agree across backends, are deterministic, and report progress");
        {
            const auto file = writeRandomFile (temp.get().getChildFile ("song.wav"), 3 * 1024 * 1024 + 17, 7);
            juce::String first;

            for (const auto backend : allBackends)
            {
                juce::int64 lastDone = 0;
                auto monotonic = true;
                auto reports = 0;

                const auto outcome = hashFile (
                    file,
                    [&] (juce::int64 done, juce::int64 total)
                    {
                        monotonic = monotonic && done >= lastDone && total == file.getSize();
                        lastDone = done;
                        ++reports;
                        return true;
                    },
                    backend);

                expect (outcome.status == HashOutcome::Status::completed, outcome.error);
                expectEquals (outcome.bytesHashed, file.getSize());
                expect (monotonic && reports >= 3, "progress reports: " + juce::String (reports));
                expectEquals (lastDone, file.getSize());

                if (first.isEmpty())
                    first = outcome.sha256Hex;
                else
                    expectEquals (outcome.sha256Hex, first, "backends agree");
            }

            expectEquals (hashFile (file, {}).sha256Hex, first, "deterministic");
            expect (hashFile (temp.get().getChildFile ("missing.wav"), {}).status == HashOutcome::Status::failed);
        }

        beginTest ("Hashing cancels promptly");
        {
            const auto file = writeRandomFile (temp.get().getChildFile ("cancel.wav"), 8 * 1024 * 1024, 8);

            for (const auto backend : allBackends)
            {
                auto calls = 0;
                const auto outcome = hashFile (file, [&] (juce::int64, juce::int64) { return ++calls < 2; }, backend);
                expect (outcome.status == HashOutcome::Status::cancelled, toString (backend));
                expect (outcome.bytesHashed < file.getSize());
                expect (outcome.sha256Hex.isEmpty());
            }

            FingerprintMemo memo (temp.get().getChildFile ("settings/cancel.json"));
            const auto outcome = fingerprintSource (file, &memo, [] (juce::int64, juce::int64) { return false; });
            expect (outcome.status == FingerprintOutcome::Status::cancelled);
            expectEquals (memo.getNumEntries(), 0, "cancelled hashes are never memoised");
        }

        beginTest ("Fingerprints identify content and size, not the path");
        {
            const auto a = writeRandomFile (temp.get().getChildFile ("a/one.mp3"), 100000, 9);
            const auto b = temp.get().getChildFile ("b/renamed.mp3");
            b.getParentDirectory().createDirectory();
            expect (a.copyFileTo (b));

            const auto fa = fingerprintSource (a, nullptr, {});
            const auto fb = fingerprintSource (b, nullptr, {});
            expect (fa.status == FingerprintOutcome::Status::completed);
            expect (fa.fingerprint == fb.fingerprint);
            expect (fa.fingerprint.isValid());
            expectEquals (fa.fingerprint.sizeBytes, static_cast<juce::int64> (100000));
            expect (fa.fingerprint.getContentHash().startsWith ("sha256:"));
            expectEquals (fa.fingerprint.getCacheKey(), "sha256-" + fa.fingerprint.sha256Hex + "-100000");

            {
                juce::FileOutputStream out (b);
                out.setPosition (500);
                out.writeByte ('x');
            }

            const auto changed = fingerprintSource (b, nullptr, {});
            expect (changed.fingerprint.sha256Hex != fa.fingerprint.sha256Hex, "one changed byte changes the hash");

            expect (! model::SourceFingerprint {}.isValid());
            expect (! model::SourceFingerprint { "ABC", 3 }.isValid());
        }

        beginTest ("Memo fast path: reuse, persistence, and invalidation on size or time changes");
        {
            const auto storage = temp.get().getChildFile ("settings/fingerprints.json");
            const auto file = writeRandomFile (temp.get().getChildFile ("memo/song.wav"), 200000, 10);

            FingerprintMemo memo (storage);
            const auto first = fingerprintSource (file, &memo, {});
            expect (first.status == FingerprintOutcome::Status::completed && ! first.fromMemo);
            expect (storage.existsAsFile(), "memo persisted");

            const auto second = fingerprintSource (file, &memo, {});
            expect (second.fromMemo, "unchanged file reuses the verified hash");
            expect (second.fingerprint == first.fingerprint);

            FingerprintMemo reloaded (storage);
            expect (fingerprintSource (file, &reloaded, {}).fromMemo, "memo survives a restart");

            // Same content, newer modification time: rehash (and the hash is unchanged).
            const auto modified = file.getLastModificationTime();
            expect (file.setLastModificationTime (modified + juce::RelativeTime::seconds (5)));
            const auto touched = fingerprintSource (file, &memo, {});
            expect (! touched.fromMemo, "a touched file is rehashed");
            expect (touched.fingerprint == first.fingerprint);
            expect (fingerprintSource (file, &memo, {}).fromMemo, "and remembered again");

            // Size change with the modification time restored: rehash, new identity.
            const auto restoreTime = file.getLastModificationTime();
            expect (file.appendData ("tail", 4));
            expect (file.setLastModificationTime (restoreTime));
            const auto grown = fingerprintSource (file, &memo, {});
            expect (! grown.fromMemo, "a resized file is rehashed");
            expectEquals (grown.fingerprint.sizeBytes, static_cast<juce::int64> (200004));
            expect (grown.fingerprint.sha256Hex != first.fingerprint.sha256Hex);

            // Same bytes and times at another path: not trusted.
            const auto moved = temp.get().getChildFile ("memo/moved.wav");
            expect (file.copyFileTo (moved));
            expect (! fingerprintSource (moved, &memo, {}).fromMemo, "a different path is rehashed");
        }

        beginTest ("A corrupt memo file is ignored and rebuilt");
        {
            const auto storage = temp.createFile ("settings/corrupt.json", "{ this is not json");
            const auto file = writeRandomFile (temp.get().getChildFile ("corrupt/song.mp3"), 5000, 11);

            FingerprintMemo memo (storage);
            expect (! memo.lookup (FileStamp::capture (file)).has_value());
            expect (fingerprintSource (file, &memo, {}).status == FingerprintOutcome::Status::completed);

            const auto json = juce::JSON::parse (storage.loadFileAsString());
            expectEquals (static_cast<int> (json.getProperty ("schema", 0)), FingerprintMemo::schemaVersion);
            expectEquals (json.getProperty ("entries", {}).size(), 1);
        }

        beginTest ("The memo keeps a bounded number of entries");
        {
            FingerprintMemo memo (temp.get().getChildFile ("settings/bounded.json"));
            const model::SourceFingerprint fingerprint { juce::String::repeatedString ("ab", 32), 10 };

            for (auto i = 0; i < FingerprintMemo::maxEntries + 20; ++i)
                memo.remember ({ "C:\\music\\song" + juce::String (i) + ".wav", 10, 1000 + i, 1000 }, fingerprint);

            expectEquals (memo.getNumEntries(), FingerprintMemo::maxEntries);
            expect (! memo.lookup ({ "C:\\music\\song0.wav", 10, 1000, 1000 }).has_value(), "oldest evicted");
            expect (memo.lookup ({ "C:\\MUSIC\\SONG275.wav", 10, 1275, 1000 }).has_value(), "case-insensitive path");
            expect (! memo.lookup ({ "C:\\music\\song275.wav", 11, 1275, 1000 }).has_value(), "size must match");
            expect (! memo.lookup ({ "C:\\music\\song275.wav", 10, 1276, 1000 }).has_value(), "mtime must match");
            expect (! memo.lookup ({ "C:\\music\\song275.wav", 10, 1275, 1001 }).has_value(), "ctime must match");
        }

        beginTest ("Background fingerprinter completes and cancels off the calling thread");
        {
            const auto small = writeRandomFile (temp.get().getChildFile ("bg/small.wav"), 300000, 12);
            BackgroundFingerprinter fingerprinter (nullptr);
            juce::WaitableEvent done;
            FingerprintOutcome result;
            std::thread::id callbackThread;

            fingerprinter.start (small,
                                 [&] (const juce::File&, const FingerprintOutcome& outcome)
                                 {
                                     result = outcome;
                                     callbackThread = std::this_thread::get_id();
                                     done.signal();
                                 });

            expect (done.wait (10000), "completion arrives");
            expect (result.status == FingerprintOutcome::Status::completed, result.error);
            expect (callbackThread != std::this_thread::get_id(), "hashing ran on a background thread");
            expectEquals (fingerprinter.getProgress(), 1.0f);

            // The portable backend is slow enough to cancel mid-file reliably.
            const auto large = writeRandomFile (temp.get().getChildFile ("bg/large.wav"), 24 * 1024 * 1024, 13);
            BackgroundFingerprinter slow (nullptr, HashBackend::juceSha256);
            juce::WaitableEvent cancelledEvent;
            FingerprintOutcome cancelled;

            slow.start (large,
                        [&] (const juce::File&, const FingerprintOutcome& outcome)
                        {
                            cancelled = outcome;
                            cancelledEvent.signal();
                        });

            for (auto i = 0; i < 5000 && slow.getProgress() <= 0.0f && slow.isRunning(); ++i)
                juce::Thread::sleep (1);

            slow.cancel();
            expect (cancelledEvent.wait (10000));
            expect (cancelled.status == FingerprintOutcome::Status::cancelled, "cancel stops the job");
            expect (! slow.isRunning());
        }
    }
};

//==============================================================================
/**
    Hashing throughput on a 100 MiB temporary file for both backends. Opt-in: snds_tests --benchmark.
*/
class FingerprintBenchmark final : public juce::UnitTest
{
public:
    FingerprintBenchmark()
        : juce::UnitTest ("Fingerprint throughput", benchmarkCategory)
    {
    }

    void runTest() override
    {
        beginTest ("SHA-256 throughput on 100 MiB");

        const ScopedTempDirectory temp;
        const auto file = writeRandomFile (temp.get().getChildFile ("bench.bin"), 100LL * 1024 * 1024, 99);
        juce::String reference;

        for (const auto backend : allBackends)
        {
            hashFile (file, {}, backend); // warm the OS file cache and the code path

            const auto start = juce::Time::getMillisecondCounterHiRes();
            const auto outcome = hashFile (file, {}, backend);
            const auto seconds = (juce::Time::getMillisecondCounterHiRes() - start) / 1000.0;

            expect (outcome.status == HashOutcome::Status::completed);
            logMessage ("    " + juce::String (toString (backend)).paddedRight (' ', 22)
                        + juce::String (100.0 / seconds, 0) + " MiB/s (" + juce::String (seconds, 3) + " s)");

            if (reference.isEmpty())
                reference = outcome.sha256Hex;
            else
                expectEquals (outcome.sha256Hex, reference);
        }
    }
};

static SourceFingerprintTests sourceFingerprintTests;
static FingerprintBenchmark fingerprintBenchmark;

} // namespace snds::test
