#include "app/CacheStore.h"

#include "app/Logging.h"
#include "app/WorkerProcess.h"
#include "model/SourceFingerprint.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace snds
{
juce::int64 folderSizeBytes (const juce::File& directory)
{
    juce::int64 total = 0;

    for (const auto& file : directory.findChildFiles (juce::File::findFiles, true))
        total += file.getSize();

    return total;
}

//==============================================================================
CacheStore::ActiveJob::ActiveJob (ActiveJob&& other) noexcept
    : store (std::exchange (other.store, nullptr)),
      jobId (std::move (other.jobId))
{
}

CacheStore::ActiveJob& CacheStore::ActiveJob::operator= (ActiveJob&& other) noexcept
{
    if (this != &other)
    {
        release();
        store = std::exchange (other.store, nullptr);
        jobId = std::move (other.jobId);
    }

    return *this;
}

CacheStore::ActiveJob::~ActiveJob()
{
    release();
}

void CacheStore::ActiveJob::release()
{
    if (store == nullptr)
        return;

    const std::lock_guard<std::mutex> lock (store->mutex);
    store->activeJobs.erase (store->activeJobs.find (jobId));
    store = nullptr;
}

//==============================================================================
CacheStore::CacheStore (juce::File cache, juce::File jobs, SongCompatibility songCompatibility)
    : cacheDir (std::move (cache)),
      jobsDir (std::move (jobs)),
      compatibility (std::move (songCompatibility))
{
}

CacheStore::CacheStore (const DataPaths& paths, SongCompatibility songCompatibility)
    : CacheStore (paths.cacheDirectory(), paths.jobsDirectory(), std::move (songCompatibility))
{
}

bool CacheStore::isPlainChildName (const juce::String& name)
{
    return name.isNotEmpty() && name != "." && name != ".." && ! name.containsAnyOf ("/\\:*?\"<>|");
}

CacheStore::ActiveJob CacheStore::markJobActive (const juce::String& jobId)
{
    const std::lock_guard<std::mutex> lock (mutex);
    activeJobs.insert (jobId);
    return ActiveJob (this, jobId);
}

bool CacheStore::isActive (const juce::String& jobId) const
{
    const std::lock_guard<std::mutex> lock (mutex);
    return activeJobs.find (jobId) != activeJobs.end();
}

//==============================================================================
CacheEntry CacheStore::classify (const juce::File& directory) const
{
    CacheEntry entry;
    entry.key = directory.getFileName();
    entry.directory = directory;
    entry.sizeBytes = folderSizeBytes (directory);

    auto loaded = loadSongPackage (directory, compatibility);
    entry.status = loaded.status;
    entry.reason = loaded.reason;

    if (loaded.isComplete())
        entry.song = std::move (loaded.metadata);

    return entry;
}

std::vector<CacheEntry> CacheStore::listEntries() const
{
    std::vector<CacheEntry> entries;

    for (const auto& directory : cacheDir.findChildFiles (juce::File::findDirectories, false))
        entries.push_back (classify (directory));

    std::sort (entries.begin(),
               entries.end(),
               [] (const CacheEntry& a, const CacheEntry& b)
               {
                   if (a.isPlayable() != b.isPlayable())
                       return a.isPlayable();

                   if (a.isPlayable() && a.song->preparedAt != b.song->preparedAt)
                       return a.song->preparedAt > b.song->preparedAt;

                   return a.key < b.key;
               });

    return entries;
}

std::vector<CacheEntry> CacheStore::listPlayableEntries() const
{
    auto entries = listEntries();
    entries.erase (
        std::remove_if (entries.begin(), entries.end(), [] (const CacheEntry& e) { return ! e.isPlayable(); }),
        entries.end());
    return entries;
}

std::optional<CacheEntry> CacheStore::findEntry (const juce::String& key) const
{
    if (! isPlainChildName (key))
        return std::nullopt;

    const auto directory = cacheDir.getChildFile (key);

    if (! directory.isDirectory())
        return std::nullopt;

    return classify (directory);
}

DiskUsage CacheStore::usage() const
{
    DiskUsage result;
    result.cacheBytes = folderSizeBytes (cacheDir);
    result.jobBytes = folderSizeBytes (jobsDir);
    return result;
}

CacheVerification CacheStore::verifyEntry (const juce::String& key, const std::function<bool()>& shouldCancel) const
{
    const auto cancelled = [&] { return shouldCancel && shouldCancel(); };
    if (cancelled())
        return { false, "Verification cancelled." };

    const auto entry = findEntry (key);
    if (! entry || ! entry->isPlayable())
        return { false, "This song cannot be verified: " + (entry ? entry->reason : "entry not found.") };

    juce::WavAudioFormat format;
    juce::AudioBuffer<float> samples (2, 65536);
    bool hasAllChecksums = true;
    for (const auto& stem : entry->song->stems)
    {
        if (cancelled())
            return { false, "Verification cancelled." };
        const auto file = entry->directory.getChildFile (stem.file);
        const auto sizeBefore = file.getSize();
        const auto modifiedBefore = file.getLastModificationTime();
        auto input = file.createInputStream();
        if (input == nullptr)
            return { false, stem.file + " could not be opened." };
        std::unique_ptr<juce::AudioFormatReader> reader (format.createReaderFor (input.release(), true));
        if (reader == nullptr || reader->numChannels != 2 || reader->bitsPerSample != 32
            || ! reader->usesFloatingPointData || reader->sampleRate != stem.sampleRate
            || reader->lengthInSamples != stem.frames)
            return { false, stem.file + " has an invalid audio header. Delete the prepared song and import it again." };

        for (juce::int64 offset = 0; offset < stem.frames; offset += samples.getNumSamples())
        {
            if (cancelled())
                return { false, "Verification cancelled." };
            const auto count = static_cast<int> (std::min<juce::int64> (samples.getNumSamples(), stem.frames - offset));
            if (! reader->read (&samples, 0, count, offset, true, true))
                return { false, stem.file + " could not be read completely." };
            for (int channel = 0; channel < 2; ++channel)
                for (int sample = 0; sample < count; ++sample)
                    if (! std::isfinite (samples.getSample (channel, sample)))
                        return { false,
                                 stem.file
                                     + " contains non-finite audio. Delete the prepared song and import it again." };
        }
        reader.reset();
        if (stem.sha256.isEmpty())
            hasAllChecksums = false;
        else
        {
            const auto fingerprint = computeFingerprint (file, {}, shouldCancel);
            if (fingerprint.status == FingerprintResult::Status::cancelled)
                return { false, "Verification cancelled." };
            if (! fingerprint.ok())
                return { false, stem.file + ": " + fingerprint.error };
            if (fingerprint.fingerprint.sha256 != stem.sha256 || fingerprint.fingerprint.sizeBytes != stem.sizeBytes)
                return { false,
                         stem.file + " has changed since preparation. Delete the prepared song and import it again." };
        }
        if (file.getSize() != sizeBefore || file.getLastModificationTime() != modifiedBefore)
            return { false, stem.file + " changed during verification. Try again when it is no longer being edited." };
    }
    return { true,
             hasAllChecksums ? "Verified: all stem checksums, headers and audio samples passed."
                             : "Audio checks passed. This older song has no saved checksum for every stem; unchanged "
                               "content cannot be confirmed. Reimport after deleting it to save checksums." };
}

//==============================================================================
juce::Result CacheStore::removeFolder (const juce::File& directory)
{
    // song.json goes first: if the rest cannot be deleted (a stem held open by a player), the
    // leftovers are still not a complete package.
    directory.getChildFile ("song.json").deleteFile();

    if (directory.deleteRecursively())
        return juce::Result::ok();

    return juce::Result::fail ("Some files in " + directory.getFileName()
                               + " could not be deleted. Close any program that is using them and try again.");
}

juce::Result CacheStore::deleteEntry (const juce::String& key)
{
    if (! isPlainChildName (key))
        return juce::Result::fail ("\"" + key + "\" is not a cache entry.");

    const auto directory = cacheDir.getChildFile (key);

    if (! directory.isDirectory())
        return juce::Result::fail ("The cache entry " + key + " no longer exists.");

    logInfo ("Deleting cache entry " + key);
    return removeFolder (directory);
}

juce::Result CacheStore::removeNonPlayableEntry (const juce::String& key)
{
    const auto entry = findEntry (key);

    if (! entry)
        return juce::Result::ok();

    if (entry->isPlayable())
        return juce::Result::fail ("The cache entry " + key + " is complete; it was not replaced.");

    logInfo ("Removing " + SongLoadResult::statusName (entry->status) + " cache entry " + key + ": " + entry->reason);
    return removeFolder (entry->directory);
}

juce::Result CacheStore::clearAll()
{
    juce::String firstProblem;

    for (const auto& directory : cacheDir.findChildFiles (juce::File::findDirectories, false))
    {
        logInfo ("Clearing cache entry " + directory.getFileName());

        if (const auto result = removeFolder (directory); result.failed() && firstProblem.isEmpty())
            firstProblem = result.getErrorMessage();
    }

    return firstProblem.isEmpty() ? juce::Result::ok() : juce::Result::fail (firstProblem);
}

//==============================================================================
std::vector<JobFolder> CacheStore::listJobs() const
{
    std::vector<JobFolder> jobs;

    for (const auto& directory : jobsDir.findChildFiles (juce::File::findDirectories, false))
    {
        JobFolder job;
        job.jobId = directory.getFileName();
        job.directory = directory;
        job.sizeBytes = folderSizeBytes (directory);
        job.active = isActive (job.jobId);
        jobs.push_back (std::move (job));
    }

    std::sort (jobs.begin(), jobs.end(), [] (const JobFolder& a, const JobFolder& b) { return a.jobId < b.jobId; });
    return jobs;
}

QuarantineReport CacheStore::quarantineAbandonedJobs()
{
    QuarantineReport report;

    for (const auto& job : listJobs())
    {
        // Re-check at the moment of cleaning: a job may have started since the listing.
        if (job.active || isActive (job.jobId))
        {
            ++report.skippedActive;
            continue;
        }

        const auto before = job.sizeBytes;
        bool clean = true;

        const auto package = job.directory.getChildFile ("package");

        if (package.exists() && ! package.deleteRecursively())
        {
            clean = false;
            report.problems.add (job.jobId + ": package could not be deleted");
        }

        deleteIncompleteAudio (job.directory);

        if (! clean)
            continue;

        const auto freed = before - folderSizeBytes (job.directory);

        if (freed > 0)
        {
            report.bytesFreed += freed;
            ++report.foldersCleaned;
            logInfo ("Quarantined abandoned job " + job.jobId + " (" + juce::String (freed)
                     + " bytes of audio removed)");
        }
    }

    return report;
}

juce::Result CacheStore::deleteJobFolder (const juce::String& jobId)
{
    if (! isPlainChildName (jobId))
        return juce::Result::fail ("\"" + jobId + "\" is not a job folder.");

    if (isActive (jobId))
        return juce::Result::fail ("The job " + jobId + " is still running.");

    const auto directory = jobsDir.getChildFile (jobId);

    if (! directory.isDirectory())
        return juce::Result::ok();

    return directory.deleteRecursively()
             ? juce::Result::ok()
             : juce::Result::fail ("Some files in the job folder " + jobId + " could not be deleted.");
}
} // namespace snds
