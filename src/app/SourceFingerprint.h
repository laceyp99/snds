#pragma once

#include "model/SongSource.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

/**
    Source fingerprinting: SHA-256 over every byte of the source file plus its size (the cache
    identity), computed on a background thread with progress and cancellation, and a persisted
    fast-path memo so reopening an unchanged file does not rehash it.

    Threading: hashFile() and fingerprintSource() block on file I/O for as long as the file takes
    to read (seconds for a large WAV), so they must never run on the message thread.
    BackgroundFingerprinter owns a private thread for them. FingerprintMemo is thread-safe.
*/
namespace snds
{

/**
    Receives hashing progress (bytes done, total bytes) roughly once per MiB, on the hashing thread.
    Return false to cancel.
*/
using HashProgress = std::function<bool (juce::int64 bytesDone, juce::int64 totalBytes)>;

enum class HashBackend
{
    windowsCng, ///< Windows CNG (BCrypt): hardware SHA extensions where available. Default on Windows.
    juceSha256, ///< juce::SHA256: portable scalar code. Default elsewhere; also used for cross-checks.
};

const char* toString (HashBackend backend) noexcept;
HashBackend getDefaultHashBackend() noexcept;

struct HashOutcome
{
    enum class Status
    {
        completed,
        cancelled,
        failed,
    };

    Status status = Status::failed;
    juce::String sha256Hex; ///< 64 lowercase hex digits when completed.
    juce::int64 bytesHashed = 0;
    juce::String error;
};

/** SHA-256 of a whole file, read in 1 MiB blocks. Blocks the calling thread. */
HashOutcome
hashFile (const juce::File& file, const HashProgress& progress, HashBackend backend = getDefaultHashBackend());

/** SHA-256 of a memory buffer as lowercase hex (known-answer tests and small data). */
juce::String sha256Hex (const void* data, size_t numBytes, HashBackend backend = getDefaultHashBackend());

//==============================================================================
/** What the fast path compares to decide that a file is unchanged. */
struct FileStamp
{
    juce::String path;          ///< Full path.
    juce::int64 sizeBytes = -1; ///< -1 when the file does not exist.
    juce::int64 modifiedMs = 0; ///< Last write time, milliseconds since the epoch.
    juce::int64 createdMs = 0;  ///< Creation time, milliseconds since the epoch.

    static FileStamp capture (const juce::File& file);

    bool isValid() const noexcept { return sizeBytes >= 0 && path.isNotEmpty(); }

    /** Same file and same size and times. Paths compare case-insensitively where the OS does. */
    bool matches (const FileStamp& other) const;
};

/**
    Persisted memo of verified fingerprints (settings/fingerprints.json) that lets an unchanged file
    skip rehashing.

    When the fast path is trusted:
    - Only hashes SNDS computed itself, over the whole file, are recorded, and only when the file's
      stamp was identical before and after hashing (a file being written is never memoised).
    - A hit needs the same full path, the same size in bytes, and the same last-write and creation
      times to the millisecond. A moved, copied, edited, touched, or restored file is rehashed.
    - A corrupt or missing memo file is treated as empty; it is advisory and is rebuilt as files
      are hashed. At most maxEntries entries are kept (oldest verification dropped first).

    Known blind spot: a tool that rewrites bytes in place without changing the size and then
    restores both timestamps (for example a tag editor with "preserve file times" rewriting
    padding) defeats the stamp. Deleting fingerprints.json forces full hashing.
*/
class FingerprintMemo
{
public:
    static constexpr int maxEntries = 256;
    static constexpr const char* defaultFileName = "fingerprints.json";
    static constexpr int schemaVersion = 1;

    explicit FingerprintMemo (juce::File storageFile);

    std::optional<model::SourceFingerprint> lookup (const FileStamp& stamp);

    /** Records a verified fingerprint for the stamp, replacing any entry for the same path, and saves. */
    void remember (const FileStamp& stamp, const model::SourceFingerprint& fingerprint);

    int getNumEntries();
    const juce::File& getStorageFile() const noexcept { return storageFile; }

private:
    struct Entry
    {
        FileStamp stamp;
        model::SourceFingerprint fingerprint;
        juce::int64 verifiedMs = 0;
    };

    void loadLocked();
    bool saveLocked() const;

    const juce::File storageFile;
    std::mutex mutex;
    std::vector<Entry> entries; ///< Oldest verification first.
    bool loaded = false;

    JUCE_DECLARE_NON_COPYABLE (FingerprintMemo)
};

//==============================================================================
struct FingerprintOutcome
{
    enum class Status
    {
        completed,
        cancelled,
        failed,
    };

    Status status = Status::failed;
    model::SourceFingerprint fingerprint;
    bool fromMemo = false;
    double hashSeconds = 0.0; ///< Time spent hashing (0 for a memo hit).
    juce::String error;       ///< User-facing reason when failed.
};

/**
    Fingerprints a source: memo fast path first, otherwise a full hash with progress and
    cancellation, a check that the file did not change while it was read, and a memo update.
    memo may be null. Blocks the calling thread.
*/
FingerprintOutcome fingerprintSource (const juce::File& file,
                                      FingerprintMemo* memo,
                                      const HashProgress& progress,
                                      HashBackend backend = getDefaultHashBackend());

/**
    Runs fingerprintSource() on a private background thread with observable progress and
    cancellation. One job at a time: start() cancels the previous job first.
*/
class BackgroundFingerprinter
{
public:
    /** Runs on the background thread for every job, including cancelled and failed ones. */
    using Completion = std::function<void (const juce::File&, const FingerprintOutcome&)>;

    explicit BackgroundFingerprinter (std::shared_ptr<FingerprintMemo> memo,
                                      HashBackend backend = getDefaultHashBackend());
    ~BackgroundFingerprinter();

    void start (const juce::File& file, Completion onComplete);

    /** Requests cancellation of the running job and waits for it to stop. */
    void cancel();

    bool isRunning() const noexcept { return running.load(); }

    /** Progress of the current (or last) job, 0 to 1. */
    float getProgress() const noexcept { return progress.load(); }

private:
    class Job;

    std::shared_ptr<FingerprintMemo> memo;
    const HashBackend backend;
    std::atomic<float> progress { 0.0f };
    std::atomic<bool> running { false };

    // Declared last so it is destroyed first: waits for the job before the members above go away.
    juce::ThreadPool pool { juce::ThreadPoolOptions {}.withThreadName ("SNDS fingerprint").withNumberOfThreads (1) };

    JUCE_DECLARE_NON_COPYABLE (BackgroundFingerprinter)
};

} // namespace snds
