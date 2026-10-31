#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <memory>
#include <thread>

namespace snds
{
/** Incremental SHA-256 (Windows CNG). Feed bytes in any chunking; finish() gives 64 lowercase
    hex characters and ends the hash. */
class Sha256
{
public:
    Sha256();
    ~Sha256();

    /** False if the system crypto provider could not be opened; update/finish then do nothing. */
    bool isValid() const noexcept;

    void update (const void* data, size_t size);
    juce::String finish();

    /** One-shot convenience. */
    static juce::String hash (const void* data, size_t size);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE (Sha256)
};

/** Identity of a source file: the content hash and the byte size. */
struct SourceFingerprint
{
    juce::String sha256;
    juce::int64 sizeBytes = 0;

    /** `<sha256>-<sizeBytes>`: the cache folder name (docs/protocol.md section 4). */
    juce::String cacheKey() const;

    bool operator== (const SourceFingerprint& other) const noexcept
    {
        return sizeBytes == other.sizeBytes && sha256 == other.sha256;
    }
};

struct FingerprintResult
{
    enum class Status
    {
        ok,
        cancelled,
        unreadable,
        changedWhileReading
    };

    Status status = Status::unreadable;
    SourceFingerprint fingerprint; // meaningful only when ok
    juce::String error;            // for the user; empty when ok or cancelled

    bool ok() const noexcept { return status == Status::ok; }
};

/** Hashes a file, blocking the calling thread. Reads in 1 MiB chunks; `progress` (bytes done,
    total) is called at most every 50 ms and once at the end; `shouldCancel` is polled before each
    chunk. Run it on a background thread.
*/
FingerprintResult computeFingerprint (const juce::File& file,
                                      const std::function<void (juce::int64, juce::int64)>& progress = {},
                                      const std::function<bool()>& shouldCancel = {});

/** computeFingerprint on its own thread, with results delivered to the message thread (or the
    given dispatcher), in order, and never after destruction. Destroying the object cancels the
    work within one chunk read.
*/
class FingerprintJob
{
public:
    using Dispatcher = std::function<void (std::function<void()>)>;

    FingerprintJob (juce::File file,
                    std::function<void (juce::int64 done, juce::int64 total)> onProgress,
                    std::function<void (const FingerprintResult&)> onFinished,
                    Dispatcher dispatcher = {});
    ~FingerprintJob();

    /** Asks the work to stop; onFinished then reports Status::cancelled. Non-blocking. */
    void cancel();

    bool isRunning() const;

private:
    struct Shared;
    std::shared_ptr<Shared> shared;
    std::thread worker;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FingerprintJob)
};
} // namespace snds
