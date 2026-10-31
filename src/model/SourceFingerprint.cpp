#include "model/SourceFingerprint.h"

#include <juce_events/juce_events.h>

#include "model/SongMetadata.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// bcrypt.h needs the types windows.h defines, so it is its own include block.

#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <vector>

namespace snds
{
namespace
{
constexpr size_t chunkBytes = 1024 * 1024;
constexpr double progressIntervalMs = 50.0;
constexpr ULONG digestBytes = 32;

bool succeeded (NTSTATUS status)
{
    return status >= 0;
}
} // namespace

//==============================================================================
struct Sha256::Impl
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool valid = false;

    Impl()
    {
        valid = succeeded (BCryptOpenAlgorithmProvider (&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))
             && succeeded (BCryptCreateHash (algorithm, &hash, nullptr, 0, nullptr, 0, 0));
    }

    ~Impl()
    {
        if (hash != nullptr)
            BCryptDestroyHash (hash);

        if (algorithm != nullptr)
            BCryptCloseAlgorithmProvider (algorithm, 0);
    }
};

Sha256::Sha256()
    : impl (std::make_unique<Impl>())
{
}

Sha256::~Sha256() = default;

bool Sha256::isValid() const noexcept
{
    return impl->valid;
}

void Sha256::update (const void* data, size_t size)
{
    if (! impl->valid)
        return;

    auto* bytes = static_cast<PUCHAR> (const_cast<void*> (data));

    while (size > 0)
    {
        const auto part = static_cast<ULONG> (std::min<size_t> (size, 1u << 30));

        if (! succeeded (BCryptHashData (impl->hash, bytes, part, 0)))
        {
            impl->valid = false;
            return;
        }

        bytes += part;
        size -= part;
    }
}

juce::String Sha256::finish()
{
    if (! impl->valid)
        return {};

    UCHAR digest[digestBytes] {};
    const auto ok = succeeded (BCryptFinishHash (impl->hash, digest, digestBytes, 0));
    impl->valid = false; // a finished hash cannot take more data

    return ok ? juce::String::toHexString (digest, static_cast<int> (digestBytes), 0) : juce::String();
}

juce::String Sha256::hash (const void* data, size_t size)
{
    Sha256 sha;
    sha.update (data, size);
    return sha.finish();
}

//==============================================================================
juce::String SourceFingerprint::cacheKey() const
{
    return SongMetadata::cacheKey (sha256, sizeBytes);
}

FingerprintResult computeFingerprint (const juce::File& file,
                                      const std::function<void (juce::int64, juce::int64)>& progress,
                                      const std::function<bool()>& shouldCancel)
{
    FingerprintResult result;

    const auto fail = [&] (FingerprintResult::Status status, const juce::String& message)
    {
        result.status = status;
        result.error = message;
        return result;
    };

    juce::FileInputStream stream (file);

    if (stream.failedToOpen())
        return fail (FingerprintResult::Status::unreadable,
                     file.getFileName()
                         + " could not be opened. Check that it exists and is not locked by another program.");

    const auto total = stream.getTotalLength();
    Sha256 sha;

    if (! sha.isValid())
        return fail (FingerprintResult::Status::unreadable, "The Windows crypto provider is not available.");

    std::vector<char> buffer (chunkBytes);
    juce::int64 done = 0;
    auto lastReport = juce::Time::getMillisecondCounterHiRes();

    while (done < total)
    {
        if (shouldCancel && shouldCancel())
        {
            result.status = FingerprintResult::Status::cancelled;
            return result;
        }

        const auto wanted =
            static_cast<int> (std::min<juce::int64> (total - done, static_cast<juce::int64> (buffer.size())));
        const auto got = stream.read (buffer.data(), wanted);

        if (got <= 0)
            return fail (FingerprintResult::Status::unreadable,
                         file.getFileName()
                             + " could not be read to the end (it may have been truncated or disconnected).");

        sha.update (buffer.data(), static_cast<size_t> (got));
        done += got;

        const auto now = juce::Time::getMillisecondCounterHiRes();

        if (progress && done < total && now - lastReport >= progressIntervalMs)
        {
            progress (done, total);
            lastReport = now;
        }
    }

    if (shouldCancel && shouldCancel())
    {
        result.status = FingerprintResult::Status::cancelled;
        return result;
    }

    if (file.getSize() != total)
        return fail (FingerprintResult::Status::changedWhileReading,
                     file.getFileName()
                         + " changed while it was being read. Wait for the other program to finish writing it, "
                           "then try again.");

    if (progress)
        progress (done, total);

    result.fingerprint.sha256 = sha.finish();
    result.fingerprint.sizeBytes = total;
    result.status =
        result.fingerprint.sha256.isNotEmpty() ? FingerprintResult::Status::ok : FingerprintResult::Status::unreadable;

    if (! result.ok())
        result.error = "The checksum could not be computed.";

    return result;
}

//==============================================================================
struct FingerprintJob::Shared : std::enable_shared_from_this<FingerprintJob::Shared>
{
    juce::File file;
    std::function<void (juce::int64, juce::int64)> onProgress;
    std::function<void (const FingerprintResult&)> onFinished;
    Dispatcher dispatcher;

    std::atomic<bool> cancelRequested { false };
    std::atomic<bool> detached { false };
    std::atomic<bool> finished { false };

    void post (std::function<void()> work)
    {
        dispatcher (
            [self = shared_from_this(), work = std::move (work)]
            {
                if (! self->detached)
                    work();
            });
    }

    void run()
    {
        auto result = computeFingerprint (
            file,
            [this] (juce::int64 done, juce::int64 total)
            {
                post (
                    [this, done, total]
                    {
                        if (onProgress)
                            onProgress (done, total);
                    });
            },
            [this] { return cancelRequested.load(); });

        finished = true;
        post (
            [this, result]
            {
                if (onFinished)
                    onFinished (result);
            });
    }
};

FingerprintJob::FingerprintJob (juce::File file,
                                std::function<void (juce::int64, juce::int64)> onProgress,
                                std::function<void (const FingerprintResult&)> onFinished,
                                Dispatcher dispatcher)
    : shared (std::make_shared<Shared>())
{
    shared->file = std::move (file);
    shared->onProgress = std::move (onProgress);
    shared->onFinished = std::move (onFinished);
    shared->dispatcher = dispatcher != nullptr ? std::move (dispatcher)
                                               : Dispatcher ([] (std::function<void()> work)
                                                             { juce::MessageManager::callAsync (std::move (work)); });
    worker = std::thread ([state = shared.get()] { state->run(); });
}

FingerprintJob::~FingerprintJob()
{
    shared->detached = true;
    shared->cancelRequested = true;
    worker.join();
}

void FingerprintJob::cancel()
{
    shared->cancelRequested = true;
}

bool FingerprintJob::isRunning() const
{
    return ! shared->finished;
}
} // namespace snds
