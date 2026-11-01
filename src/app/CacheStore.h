#pragma once

#include <juce_core/juce_core.h>

#include "app/DataPaths.h"
#include "model/SongMetadata.h"

#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace snds
{
/** One folder under SNDS_HOME\cache, classified. Only a `complete` entry is playable. */
struct CacheEntry
{
    juce::String key; // the folder name
    juce::File directory;
    juce::int64 sizeBytes = 0; // everything under the folder
    SongLoadResult::Status status = SongLoadResult::Status::corrupt;
    juce::String reason;              // why it is not complete; empty when complete
    std::optional<SongMetadata> song; // present only when complete

    bool isPlayable() const noexcept { return status == SongLoadResult::Status::complete && song.has_value(); }
};

/** One folder under SNDS_HOME\jobs. */
struct JobFolder
{
    juce::String jobId; // the folder name
    juce::File directory;
    juce::int64 sizeBytes = 0;
    bool active = false; // owned by a running job: never touch it
};

struct DiskUsage
{
    juce::int64 cacheBytes = 0;
    juce::int64 jobBytes = 0;

    juce::int64 totalBytes() const noexcept { return cacheBytes + jobBytes; }
};

/** Explicit integrity scan; older packages can pass audio checks without saved checksums. */
struct CacheVerification
{
    bool ok = false;
    juce::String message;
};

struct QuarantineReport
{
    int foldersCleaned = 0;
    int skippedActive = 0;
    juce::int64 bytesFreed = 0;
    juce::StringArray problems; // folders that could not be fully cleaned
};

/** The song cache and the temporary job folders under SNDS_HOME.

    Everything here touches the disk and may take a while for a large cache: call it from a
    background thread and hand the results to the UI. Methods are thread-safe with respect to the
    set of active jobs; they do not lock the files themselves, so the caller keeps one cleanup
    running at a time.

    A cache entry is playable only if loadSongPackage says `complete`. Deleting an entry removes
    its song.json first, so even a delete that fails halfway (a stem locked by a player) leaves
    nothing that could be mistaken for a complete package.
*/
class CacheStore
{
public:
    CacheStore (juce::File cacheDirectory, juce::File jobsDirectory, SongCompatibility compatibility = {});

    explicit CacheStore (const DataPaths& paths, SongCompatibility compatibility = {});

    const juce::File& cacheDirectory() const noexcept { return cacheDir; }
    const juce::File& jobsDirectory() const noexcept { return jobsDir; }

    /** Every folder in the cache with its status and size: complete entries newest first
        (by preparedAt), then the others by name. */
    std::vector<CacheEntry> listEntries() const;

    /** The playable entries only. */
    std::vector<CacheEntry> listPlayableEntries() const;

    /** One entry by folder name, if the folder exists. */
    std::optional<CacheEntry> findEntry (const juce::String& key) const;

    DiskUsage usage() const;

    /** Checks every stem's header, samples and saved checksum without modifying files.
        Call on a background thread; cancellation is checked between bounded reads. */
    CacheVerification verifyEntry (const juce::String& key, const std::function<bool()>& shouldCancel = {}) const;

    /** Deletes one entry (any status). Fails for names that are not a direct child folder of the
        cache, or when files are locked; the message says what to do. */
    juce::Result deleteEntry (const juce::String& key);

    /** Deletes an incompatible or corrupt entry so the song can be prepared again; refuses a
        complete one. Succeeds if there is nothing to delete. */
    juce::Result removeNonPlayableEntry (const juce::String& key);

    /** Deletes every entry folder. The caller has confirmed with the user. Job folders are left
        alone. Returns the first problem, after trying all entries. */
    juce::Result clearAll();

    // --- job folders -----------------------------------------------------------------------

    /** Marks a job folder as owned by a running worker for the lifetime of the returned token. */
    class ActiveJob
    {
    public:
        ActiveJob() = default;
        ActiveJob (ActiveJob&& other) noexcept;
        ActiveJob& operator= (ActiveJob&& other) noexcept;
        ~ActiveJob();

    private:
        friend class CacheStore;
        ActiveJob (CacheStore* owner, juce::String id)
            : store (owner),
              jobId (std::move (id))
        {
        }
        void release();

        CacheStore* store = nullptr;
        juce::String jobId;

        ActiveJob (const ActiveJob&) = delete;
        ActiveJob& operator= (const ActiveJob&) = delete;
    };

    /** Call this before creating the job folder, so a quarantine pass can never see the folder
        without the mark. The store must outlive the token. */
    [[nodiscard]] ActiveJob markJobActive (const juce::String& jobId);

    std::vector<JobFolder> listJobs() const;

    /** For every job folder not owned by a running job: deletes `package` and any audio, keeps
        logs and the app's own files (request, staged artwork). Idempotent. */
    QuarantineReport quarantineAbandonedJobs();

    /** Deletes a whole job folder, logs included. Refuses an active job. */
    juce::Result deleteJobFolder (const juce::String& jobId);

private:
    bool isActive (const juce::String& jobId) const;
    CacheEntry classify (const juce::File& directory) const;
    static juce::Result removeFolder (const juce::File& directory);
    static bool isPlainChildName (const juce::String& name);

    juce::File cacheDir, jobsDir;
    SongCompatibility compatibility;

    mutable std::mutex mutex; // guards activeJobs
    std::multiset<juce::String> activeJobs;

    JUCE_DECLARE_NON_COPYABLE (CacheStore)
};

/** Total size of every file under a folder. */
juce::int64 folderSizeBytes (const juce::File& directory);
} // namespace snds
