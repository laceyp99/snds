#include "app/SourceFingerprint.h"

#include <juce_cryptography/juce_cryptography.h>

#if JUCE_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// windows.h must precede bcrypt.h, which relies on its base types; the blank line stops include sorting.
#include <windows.h>

#include <bcrypt.h>
#endif

namespace snds
{

namespace
{
constexpr int readBlockBytes = 1024 * 1024;
constexpr int digestBytes = 32;

HashOutcome failedHash (const juce::String& error)
{
    HashOutcome outcome;
    outcome.error = error;
    return outcome;
}

/**
    Forwards reads and reports progress once per MiB. After a cancel it reports end of stream, which
    makes juce::SHA256 finish early; the caller discards that digest.
*/
class ProgressInputStream final : public juce::InputStream
{
public:
    ProgressInputStream (juce::InputStream& sourceIn, juce::int64 totalIn, const HashProgress& progressIn)
        : source (sourceIn),
          total (totalIn),
          progress (progressIn)
    {
    }

    juce::int64 getTotalLength() override { return total; }
    bool isExhausted() override { return cancelled || source.isExhausted(); }
    juce::int64 getPosition() override { return position; }
    bool setPosition (juce::int64) override { return false; }

    int read (void* destBuffer, int maxBytesToRead) override
    {
        if (cancelled)
            return 0;

        const auto numRead = source.read (destBuffer, maxBytesToRead);

        if (numRead <= 0)
            return numRead;

        position += numRead;

        if (progress && position >= nextReport)
        {
            nextReport = position + readBlockBytes;
            cancelled = ! progress (position, total);
        }

        return numRead;
    }

    bool wasCancelled() const noexcept { return cancelled; }

private:
    juce::InputStream& source;
    const juce::int64 total;
    const HashProgress& progress;
    juce::int64 position = 0;
    juce::int64 nextReport = readBlockBytes;
    bool cancelled = false;
};

HashOutcome hashWithJuce (juce::InputStream& input, juce::int64 total, const HashProgress& progress)
{
    juce::BufferedInputStream buffered (&input, readBlockBytes, false);
    ProgressInputStream counted (buffered, total, progress);
    const juce::SHA256 digest (counted);

    HashOutcome outcome;
    outcome.bytesHashed = counted.getPosition();

    if (counted.wasCancelled())
    {
        outcome.status = HashOutcome::Status::cancelled;
        return outcome;
    }

    if (progress)
        progress (outcome.bytesHashed, total);

    outcome.status = HashOutcome::Status::completed;
    outcome.sha256Hex = digest.toHexString();
    return outcome;
}

#if JUCE_WINDOWS
/** Owns a CNG SHA-256 hash object. */
class CngSha256
{
public:
    CngSha256()
    {
        if (! BCRYPT_SUCCESS (BCryptOpenAlgorithmProvider (&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
            algorithm = nullptr;
        else if (! BCRYPT_SUCCESS (BCryptCreateHash (algorithm, &hash, nullptr, 0, nullptr, 0, 0)))
            hash = nullptr;
    }

    ~CngSha256()
    {
        if (hash != nullptr)
            BCryptDestroyHash (hash);

        if (algorithm != nullptr)
            BCryptCloseAlgorithmProvider (algorithm, 0);
    }

    bool isValid() const noexcept { return hash != nullptr; }

    bool update (const void* data, size_t numBytes)
    {
        // CNG takes a non-const pointer but does not modify the input.
        return BCRYPT_SUCCESS (
            BCryptHashData (hash, static_cast<PUCHAR> (const_cast<void*> (data)), static_cast<ULONG> (numBytes), 0));
    }

    juce::String finish()
    {
        UCHAR digest[digestBytes] {};

        if (! BCRYPT_SUCCESS (BCryptFinishHash (hash, digest, digestBytes, 0)))
            return {};

        return juce::String::toHexString (digest, digestBytes, 0);
    }

private:
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;

    JUCE_DECLARE_NON_COPYABLE (CngSha256)
};

HashOutcome hashWithCng (juce::InputStream& input, juce::int64 total, const HashProgress& progress)
{
    CngSha256 sha;

    if (! sha.isValid())
        return failedHash ("Windows could not provide SHA-256 (CNG).");

    juce::HeapBlock<char> block (readBlockBytes);
    HashOutcome outcome;

    for (;;)
    {
        const auto numRead = input.read (block.getData(), readBlockBytes);

        if (numRead < 0)
            return failedHash ("Reading the file failed.");

        if (numRead == 0)
            break;

        if (! sha.update (block.getData(), static_cast<size_t> (numRead)))
            return failedHash ("SHA-256 hashing failed.");

        outcome.bytesHashed += numRead;

        if (progress && ! progress (outcome.bytesHashed, total))
        {
            outcome.status = HashOutcome::Status::cancelled;
            return outcome;
        }
    }

    outcome.sha256Hex = sha.finish();

    if (outcome.sha256Hex.isEmpty())
        return failedHash ("SHA-256 hashing failed.");

    outcome.status = HashOutcome::Status::completed;
    return outcome;
}
#endif

HashOutcome hashStream (juce::InputStream& input, juce::int64 total, const HashProgress& progress, HashBackend backend)
{
#if JUCE_WINDOWS
    if (backend == HashBackend::windowsCng)
        return hashWithCng (input, total, progress);
#else
    juce::ignoreUnused (backend);
#endif

    return hashWithJuce (input, total, progress);
}

bool pathsEqual (const juce::String& a, const juce::String& b)
{
    return juce::File::areFileNamesCaseSensitive() ? a == b : a.equalsIgnoreCase (b);
}
} // namespace

//==============================================================================
const char* toString (HashBackend backend) noexcept
{
    switch (backend)
    {
        case HashBackend::windowsCng:
            return "Windows CNG SHA-256";
        case HashBackend::juceSha256:
            return "juce::SHA256";
    }

    return "unknown";
}

HashBackend getDefaultHashBackend() noexcept
{
#if JUCE_WINDOWS
    return HashBackend::windowsCng;
#else
    return HashBackend::juceSha256;
#endif
}

HashOutcome hashFile (const juce::File& file, const HashProgress& progress, HashBackend backend)
{
    juce::FileInputStream input (file);

    if (input.failedToOpen())
        return failedHash ("SNDS could not open \"" + file.getFileName() + "\" for reading.");

    return hashStream (input, input.getTotalLength(), progress, backend);
}

juce::String sha256Hex (const void* data, size_t numBytes, HashBackend backend)
{
    juce::MemoryInputStream input (data, numBytes, false);
    return hashStream (input, static_cast<juce::int64> (numBytes), {}, backend).sha256Hex;
}

//==============================================================================
FileStamp FileStamp::capture (const juce::File& file)
{
    FileStamp stamp;
    stamp.path = file.getFullPathName();

    if (! file.existsAsFile())
        return stamp;

    stamp.sizeBytes = file.getSize();
    stamp.modifiedMs = file.getLastModificationTime().toMilliseconds();
    stamp.createdMs = file.getCreationTime().toMilliseconds();
    return stamp;
}

bool FileStamp::matches (const FileStamp& other) const
{
    return isValid() && sizeBytes == other.sizeBytes && modifiedMs == other.modifiedMs && createdMs == other.createdMs
           && pathsEqual (path, other.path);
}

//==============================================================================
FingerprintMemo::FingerprintMemo (juce::File storageFileIn)
    : storageFile (std::move (storageFileIn))
{
}

std::optional<model::SourceFingerprint> FingerprintMemo::lookup (const FileStamp& stamp)
{
    const std::lock_guard<std::mutex> lock (mutex);
    loadLocked();

    for (const auto& entry : entries)
        if (entry.stamp.matches (stamp) && entry.fingerprint.sizeBytes == stamp.sizeBytes)
            return entry.fingerprint;

    return std::nullopt;
}

void FingerprintMemo::remember (const FileStamp& stamp, const model::SourceFingerprint& fingerprint)
{
    if (! stamp.isValid() || ! fingerprint.isValid() || fingerprint.sizeBytes != stamp.sizeBytes)
        return;

    const std::lock_guard<std::mutex> lock (mutex);
    loadLocked();

    std::erase_if (entries, [&stamp] (const Entry& entry) { return pathsEqual (entry.stamp.path, stamp.path); });
    entries.push_back ({ stamp, fingerprint, juce::Time::currentTimeMillis() });

    if (entries.size() > static_cast<size_t> (maxEntries))
        entries.erase (entries.begin(), entries.begin() + static_cast<std::ptrdiff_t> (entries.size() - maxEntries));

    saveLocked();
}

int FingerprintMemo::getNumEntries()
{
    const std::lock_guard<std::mutex> lock (mutex);
    loadLocked();
    return static_cast<int> (entries.size());
}

void FingerprintMemo::loadLocked()
{
    if (loaded)
        return;

    loaded = true;
    entries.clear();

    if (! storageFile.existsAsFile())
        return;

    const auto root = juce::JSON::parse (storageFile.loadFileAsString());

    if (static_cast<int> (root.getProperty ("schema", 0)) != schemaVersion)
        return;

    if (const auto* list = root.getProperty ("entries", {}).getArray())
    {
        for (const auto& item : *list)
        {
            Entry entry;
            entry.stamp.path = item.getProperty ("path", {}).toString();
            entry.stamp.sizeBytes = static_cast<juce::int64> (item.getProperty ("size", -1));
            entry.stamp.modifiedMs = static_cast<juce::int64> (item.getProperty ("modified", 0));
            entry.stamp.createdMs = static_cast<juce::int64> (item.getProperty ("created", 0));
            entry.fingerprint.sha256Hex = item.getProperty ("sha256", {}).toString();
            entry.fingerprint.sizeBytes = entry.stamp.sizeBytes;
            entry.verifiedMs = static_cast<juce::int64> (item.getProperty ("verified", 0));

            if (entry.stamp.isValid() && entry.fingerprint.isValid())
                entries.push_back (std::move (entry));
        }
    }

    if (entries.size() > static_cast<size_t> (maxEntries))
        entries.erase (entries.begin(), entries.begin() + static_cast<std::ptrdiff_t> (entries.size() - maxEntries));
}

bool FingerprintMemo::saveLocked() const
{
    juce::Array<juce::var> list;

    for (const auto& entry : entries)
    {
        juce::DynamicObject::Ptr item = new juce::DynamicObject();
        item->setProperty ("path", entry.stamp.path);
        item->setProperty ("size", entry.stamp.sizeBytes);
        item->setProperty ("modified", entry.stamp.modifiedMs);
        item->setProperty ("created", entry.stamp.createdMs);
        item->setProperty ("sha256", entry.fingerprint.sha256Hex);
        item->setProperty ("verified", entry.verifiedMs);
        list.add (juce::var (item.get()));
    }

    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty ("schema", schemaVersion);
    root->setProperty ("entries", list);

    if (storageFile.getParentDirectory().createDirectory().failed())
        return false;

    // Write a sibling temporary file and swap it in, so a crash never leaves a half-written memo.
    juce::TemporaryFile temporary (storageFile);

    if (! temporary.getFile().replaceWithText (juce::JSON::toString (juce::var (root.get()))))
        return false;

    return temporary.overwriteTargetFileWithTemporary();
}

//==============================================================================
FingerprintOutcome
fingerprintSource (const juce::File& file, FingerprintMemo* memo, const HashProgress& progress, HashBackend backend)
{
    FingerprintOutcome outcome;
    const auto before = FileStamp::capture (file);

    if (! before.isValid())
    {
        outcome.error = "\"" + file.getFileName() + "\" no longer exists.";
        return outcome;
    }

    if (memo != nullptr)
    {
        if (const auto remembered = memo->lookup (before))
        {
            outcome.status = FingerprintOutcome::Status::completed;
            outcome.fingerprint = *remembered;
            outcome.fromMemo = true;

            if (progress)
                progress (before.sizeBytes, before.sizeBytes);

            return outcome;
        }
    }

    const auto startMs = juce::Time::getMillisecondCounterHiRes();
    const auto hashed = hashFile (file, progress, backend);
    outcome.hashSeconds = (juce::Time::getMillisecondCounterHiRes() - startMs) / 1000.0;

    if (hashed.status == HashOutcome::Status::cancelled)
    {
        outcome.status = FingerprintOutcome::Status::cancelled;
        return outcome;
    }

    if (hashed.status == HashOutcome::Status::failed)
    {
        outcome.error = hashed.error;
        return outcome;
    }

    if (! FileStamp::capture (file).matches (before) || hashed.bytesHashed != before.sizeBytes)
    {
        outcome.error = "\"" + file.getFileName()
                        + "\" changed while it was being read. Wait until it is no longer being written, then open "
                          "it again.";
        return outcome;
    }

    outcome.status = FingerprintOutcome::Status::completed;
    outcome.fingerprint = { hashed.sha256Hex, hashed.bytesHashed };

    if (memo != nullptr)
        memo->remember (before, outcome.fingerprint);

    return outcome;
}

//==============================================================================
class BackgroundFingerprinter::Job final : public juce::ThreadPoolJob
{
public:
    Job (BackgroundFingerprinter& ownerIn, juce::File fileIn, Completion onCompleteIn)
        : ThreadPoolJob ("SNDS fingerprint"),
          owner (ownerIn),
          file (std::move (fileIn)),
          onComplete (std::move (onCompleteIn))
    {
    }

    JobStatus runJob() override
    {
        auto outcome = fingerprintSource (
            file,
            owner.memo.get(),
            [this] (juce::int64 done, juce::int64 total)
            {
                owner.progress =
                    total > 0 ? static_cast<float> (static_cast<double> (done) / static_cast<double> (total)) : 1.0f;
                return ! shouldExit();
            },
            owner.backend);

        // A cancel that arrives after the last progress report still wins.
        if (shouldExit())
            outcome = { FingerprintOutcome::Status::cancelled, {}, false, outcome.hashSeconds, {} };

        owner.running = false;

        if (onComplete)
            onComplete (file, outcome);

        return jobHasFinished;
    }

private:
    BackgroundFingerprinter& owner;
    const juce::File file;
    const Completion onComplete;
};

BackgroundFingerprinter::BackgroundFingerprinter (std::shared_ptr<FingerprintMemo> memoIn, HashBackend backendIn)
    : memo (std::move (memoIn)),
      backend (backendIn)
{
}

BackgroundFingerprinter::~BackgroundFingerprinter()
{
    cancel();
}

void BackgroundFingerprinter::start (const juce::File& file, Completion onComplete)
{
    cancel();
    progress = 0.0f;
    running = true;
    pool.addJob (new Job (*this, file, std::move (onComplete)), true);
}

void BackgroundFingerprinter::cancel()
{
    pool.removeAllJobs (true, 30000);
    running = false;
}

} // namespace snds
