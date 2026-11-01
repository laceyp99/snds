#include <juce_core/juce_core.h>

#include "SongFixtures.h"
#include "TestHelpers.h"
#include "app/CacheStore.h"
#include "model/SourceFingerprint.h"

#include <limits>

namespace snds
{
namespace
{
using Status = SongLoadResult::Status;

class CacheStoreTests final : public juce::UnitTest
{
public:
    CacheStoreTests()
        : juce::UnitTest ("Cache store", "snds")
    {
    }

    void runTest() override
    {
        beginTest ("Setup");
        runClassification();
        runVerification();
        runDeletion();
        runJobs();
    }

private:
    static void writeFloatWave (const juce::File& file, float value, int rate = 44100)
    {
        juce::MemoryOutputStream bytes;
        bytes.write ("RIFF", 4);
        bytes.writeInt (36 + 8 * 2 * 4);
        bytes.write ("WAVEfmt ", 8);
        bytes.writeInt (16);
        bytes.writeShort (3); // IEEE float WAV
        bytes.writeShort (2);
        bytes.writeInt (rate);
        bytes.writeInt (rate * 8);
        bytes.writeShort (8);
        bytes.writeShort (32);
        bytes.write ("data", 4);
        bytes.writeInt (8 * 2 * 4);
        for (int i = 0; i < 16; ++i)
            bytes.writeFloat (value);
        file.replaceWithData (bytes.getData(), bytes.getDataSize());
    }

    static juce::File integritySong (const juce::File& cache, bool withChecksums)
    {
        const test::SongFixture fixture;
        const auto package = fixture.write (cache);
        auto document = fixture.document();
        document.getProperty ("source", {}).getDynamicObject()->setProperty ("frames", 8);
        document.getProperty ("source", {}).getDynamicObject()->setProperty ("durationSeconds", 8.0 / 44100);
        auto* stems = document.getProperty ("stems", {}).getDynamicObject();
        for (const auto* name : SongMetadata::stemNames())
        {
            const auto file = package.getChildFile (juce::String (name) + ".wav");
            writeFloatWave (file, 0.25f);
            auto* stem = stems->getProperty (name).getDynamicObject();
            stem->setProperty ("frames", 8);
            if (withChecksums)
            {
                const auto fingerprint = computeFingerprint (file);
                stem->setProperty ("sha256", fingerprint.fingerprint.sha256);
                stem->setProperty ("sizeBytes", fingerprint.fingerprint.sizeBytes);
            }
        }
        package.getChildFile ("song.json").replaceWithText (juce::JSON::toString (document));
        return package;
    }

    void runVerification()
    {
        beginTest ("Verification checks hashes and catches a same-size stem edit without changing files");
        const test::ScopedTempDirectory temp;
        const CacheStore store (temp.child ("cache"), temp.child ("jobs"));
        auto package = integritySong (store.cacheDirectory(), true);
        const auto manifest = package.getChildFile ("song.json").loadFileAsString();
        auto result = store.verifyEntry (package.getFileName());
        expect (result.ok, result.message);
        expect (result.message.contains ("checksums"), result.message);
        writeFloatWave (package.getChildFile ("drums.wav"), 0.5f);
        result = store.verifyEntry (package.getFileName());
        expect (! result.ok && result.message.contains ("changed"), result.message);
        expectEquals (package.getChildFile ("song.json").loadFileAsString(), manifest);

        beginTest ("Legacy audio is checked but not reported as checksum verified");
        package = integritySong (store.cacheDirectory(), false);
        result = store.verifyEntry (package.getFileName());
        expect (result.ok && result.message.contains ("cannot be confirmed"), result.message);

        beginTest ("Verification rejects non-finite audio and mismatched headers");
        writeFloatWave (package.getChildFile ("drums.wav"), std::numeric_limits<float>::quiet_NaN());
        result = store.verifyEntry (package.getFileName());
        expect (! result.ok && result.message.contains ("non-finite"), result.message);
        writeFloatWave (package.getChildFile ("drums.wav"), 0.25f, 48000);
        result = store.verifyEntry (package.getFileName());
        expect (! result.ok && result.message.contains ("header"), result.message);

        beginTest ("Verification is cancellable between chunks and refuses unsafe keys");
        package = integritySong (store.cacheDirectory(), true);
        int checks = 0;
        result = store.verifyEntry (package.getFileName(), [&] { return ++checks >= 3; });
        expect (! result.ok && result.message.contains ("cancelled"), result.message);
        expect (! store.verifyEntry ("..").ok);

        beginTest ("Incomplete checksum provenance is rejected while legacy documents remain valid");
        auto doc = juce::JSON::parse (package.getChildFile ("song.json").loadFileAsString());
        doc.getProperty ("stems", {})
            .getDynamicObject()
            ->getProperty ("drums")
            .getDynamicObject()
            ->removeProperty ("sizeBytes");
        package.getChildFile ("song.json").replaceWithText (juce::JSON::toString (doc));
        expect (! store.findEntry (package.getFileName())->isPlayable());
    }

    static test::SongFixture songWithSha (const juce::String& pair, juce::int64 size = 1234)
    {
        test::SongFixture fixture;
        fixture.sha256 = juce::String::repeatedString (pair, 32);
        fixture.sizeBytes = size;
        return fixture;
    }

    static std::optional<CacheEntry> find (const std::vector<CacheEntry>& entries, const juce::String& key)
    {
        for (const auto& entry : entries)
            if (entry.key == key)
                return entry;

        return std::nullopt;
    }

    void expectEntry (const std::vector<CacheEntry>& entries,
                      const test::SongFixture& fixture,
                      Status status,
                      const juce::String& reasonContains = {})
    {
        const auto entry = find (entries, fixture.key());
        expect (entry.has_value(), "entry listed: " + fixture.key());

        if (! entry)
            return;

        expect (entry->status == status,
                fixture.key() + ": " + SongLoadResult::statusName (entry->status) + " (" + entry->reason + ")");
        expect (entry->isPlayable() == (status == Status::complete));
        expect (entry->song.has_value() == (status == Status::complete));

        if (reasonContains.isNotEmpty())
            expect (entry->reason.containsIgnoreCase (reasonContains), entry->reason);
    }

    //==========================================================================
    void runClassification()
    {
        beginTest ("A successful entry is complete and playable, with its on-disk size");
        {
            const test::ScopedTempDirectory temp;
            const auto good = songWithSha ("ab");
            const auto package = good.write (temp.child ("cache"), {}, 2000);
            const CacheStore store (temp.child ("cache"), temp.child ("jobs"));

            const auto entries = store.listEntries();
            expect (entries.size() == 1);
            expectEntry (entries, good, Status::complete);

            if (! entries.empty())
            {
                expect (entries[0].sizeBytes == folderSizeBytes (package));
                expect (entries[0].sizeBytes >= 8000);
                expectEquals (entries[0].song->source.fileName, juce::String ("song.wav"));
            }

            expect (store.findEntry (good.key())->isPlayable());
            expect (! store.findEntry ("nothing-here").has_value());
            expect (! store.findEntry ("..").has_value());
        }

        beginTest ("Stale provenance is incompatible, not playable, and says why");
        {
            const test::ScopedTempDirectory temp;
            const CacheStore store (temp.child ("cache"), temp.child ("jobs"));

            const auto oldSchema = songWithSha ("01");
            oldSchema.write (temp.child ("cache"), [] (juce::DynamicObject& d) { d.setProperty ("cacheSchema", 2); });

            const auto otherModel = songWithSha ("02");
            otherModel.write (temp.child ("cache"),
                              [] (juce::DynamicObject& d)
                              { d.getProperty ("model").getDynamicObject()->setProperty ("signature", "deadbeef"); });

            const auto otherProtocol = songWithSha ("03");
            otherProtocol.write (temp.child ("cache"),
                                 [] (juce::DynamicObject& d)
                                 { d.getProperty ("worker").getDynamicObject()->setProperty ("protocol", 2); });

            const auto entries = store.listEntries();
            expectEntry (entries, oldSchema, Status::incompatible, "cacheSchema");
            expectEntry (entries, otherModel, Status::incompatible, "model");
            expectEntry (entries, otherProtocol, Status::incompatible, "protocol");
            expect (store.listPlayableEntries().empty());
        }

        beginTest ("A missing source reference, a corrupt stem, and a bad document are corrupt");
        {
            const test::ScopedTempDirectory temp;
            const CacheStore store (temp.child ("cache"), temp.child ("jobs"));

            const auto noSource = songWithSha ("11");
            noSource.write (temp.child ("cache"), [] (juce::DynamicObject& d) { d.removeProperty ("source"); });

            const auto missingStem = songWithSha ("12");
            missingStem.write (temp.child ("cache")).getChildFile ("bass.wav").deleteFile();

            const auto notJson = songWithSha ("13");
            notJson.write (temp.child ("cache")).getChildFile ("song.json").replaceWithText ("{ not json");

            const auto incomplete = songWithSha ("14");
            incomplete.write (temp.child ("cache"),
                              [] (juce::DynamicObject& d) { d.setProperty ("status", "writing"); });

            const auto wrongFolder = songWithSha ("15");
            wrongFolder.write (temp.child ("cache")).moveFileTo (temp.child ("cache\\renamed-by-hand"));

            const auto entries = store.listEntries();
            expectEntry (entries, noSource, Status::corrupt, "source");
            expectEntry (entries, missingStem, Status::corrupt, "bass");
            expectEntry (entries, notJson, Status::corrupt, "JSON");
            expectEntry (entries, incomplete, Status::corrupt, "complete");

            const auto renamed = find (entries, "renamed-by-hand");
            expect (renamed && renamed->status == Status::corrupt);
            expect (store.listPlayableEntries().empty(), "none of these may be offered for playback");
        }

        beginTest ("A folder with stems but no song.json (a crashed publish) is never playable");
        {
            const test::ScopedTempDirectory temp;
            const CacheStore store (temp.child ("cache"), temp.child ("jobs"));
            const auto fixture = songWithSha ("21");
            fixture.write (temp.child ("cache")).getChildFile ("song.json").deleteFile();

            expectEntry (store.listEntries(), fixture, Status::corrupt);
            expect (store.listPlayableEntries().empty());
        }

        beginTest ("Complete entries are listed newest first, before the broken ones");
        {
            const test::ScopedTempDirectory temp;
            const CacheStore store (temp.child ("cache"), temp.child ("jobs"));

            auto older = songWithSha ("31");
            older.preparedAt = "2026-01-01T00:00:00Z";
            older.write (temp.child ("cache"));
            auto newer = songWithSha ("32");
            newer.preparedAt = "2026-09-01T00:00:00Z";
            newer.write (temp.child ("cache"));
            songWithSha ("00").write (temp.child ("cache"), [] (juce::DynamicObject& d) { d.removeProperty ("bpm"); });

            const auto entries = store.listEntries();
            expect (entries.size() == 3);

            if (entries.size() == 3)
            {
                expectEquals (entries[0].key, newer.key());
                expectEquals (entries[1].key, older.key());
                expect (! entries[2].isPlayable());
            }
        }

        beginTest ("Missing cache and jobs folders are simply empty");
        {
            const test::ScopedTempDirectory temp;
            CacheStore store (temp.child ("nope\\cache"), temp.child ("nope\\jobs"));
            expect (store.listEntries().empty());
            expect (store.listJobs().empty());
            expect (store.usage().totalBytes() == 0);
            expect (store.quarantineAbandonedJobs().foldersCleaned == 0);
            expect (store.clearAll().wasOk());
        }
    }

    //==========================================================================
    void runDeletion()
    {
        beginTest ("Deleting one song leaves the others, and the usage follows");
        {
            const test::ScopedTempDirectory temp;
            CacheStore store (temp.child ("cache"), temp.child ("jobs"));
            const auto a = songWithSha ("41");
            const auto b = songWithSha ("42");
            a.write (temp.child ("cache"));
            b.write (temp.child ("cache"));

            const auto before = store.usage();
            expect (before.cacheBytes >= 16000);

            expect (store.deleteEntry (a.key()).wasOk());
            expect (! temp.child ("cache\\" + a.key()).exists());
            expect (store.findEntry (b.key())->isPlayable());
            expect (store.usage().cacheBytes < before.cacheBytes);
            expect (store.deleteEntry (a.key()).failed(), "deleting twice reports that it is gone");
        }

        beginTest ("Names that are not direct cache folders are refused");
        {
            const test::ScopedTempDirectory temp;
            CacheStore store (temp.child ("cache"), temp.child ("jobs"));
            const auto victim = temp.child ("precious");
            victim.createDirectory();
            victim.getChildFile ("keep.txt").replaceWithText ("keep");
            temp.child ("cache").createDirectory();

            for (const char* badName : { "..", ".", "..\\precious", "../precious", "", "C:\\Windows", "a/b" })
            {
                expect (store.deleteEntry (badName).failed(), badName);
                expect (store.removeNonPlayableEntry (badName).wasOk(), badName);
            }

            expect (victim.getChildFile ("keep.txt").existsAsFile());
            expect (temp.child ("cache").isDirectory());
        }

        beginTest ("A broken entry can be removed for re-preparation; a complete one is protected");
        {
            const test::ScopedTempDirectory temp;
            CacheStore store (temp.child ("cache"), temp.child ("jobs"));
            const auto good = songWithSha ("51");
            const auto bad = songWithSha ("52");
            good.write (temp.child ("cache"));
            bad.write (temp.child ("cache"), [] (juce::DynamicObject& d) { d.setProperty ("cacheSchema", 9); });

            expect (store.removeNonPlayableEntry (good.key()).failed());
            expect (store.findEntry (good.key())->isPlayable());

            expect (store.removeNonPlayableEntry (bad.key()).wasOk());
            expect (! store.findEntry (bad.key()).has_value());
            expect (store.removeNonPlayableEntry (bad.key()).wasOk(), "nothing to remove is fine");
        }

        beginTest ("Clear all removes every entry and leaves the jobs alone");
        {
            const test::ScopedTempDirectory temp;
            CacheStore store (temp.child ("cache"), temp.child ("jobs"));
            songWithSha ("61").write (temp.child ("cache"));
            songWithSha ("62").write (temp.child ("cache"),
                                      [] (juce::DynamicObject& d) { d.setProperty ("cacheSchema", 9); });
            temp.child ("cache\\stray-folder").createDirectory();
            temp.child ("jobs\\job1\\package").createDirectory();
            temp.child ("jobs\\job1\\worker.log").replaceWithText ("log");

            expect (store.clearAll().wasOk());
            expect (store.listEntries().empty());
            expect (temp.child ("cache").isDirectory());
            expect (temp.child ("jobs\\job1\\worker.log").existsAsFile());
            expect (store.usage().cacheBytes == 0);
        }

        beginTest ("A locked file leaves nothing playable and reports how to fix it");
        {
            const test::ScopedTempDirectory temp;
            CacheStore store (temp.child ("cache"), temp.child ("jobs"));
            const auto fixture = songWithSha ("71");
            const auto package = fixture.write (temp.child ("cache"));

            {
                juce::FileInputStream holdingOpen (package.getChildFile ("drums.wav"));
                expect (holdingOpen.openedOk());
                const auto result = store.deleteEntry (fixture.key());
                // Windows refuses to delete a file held open without share-delete.
                expect (result.failed() || ! package.getChildFile ("drums.wav").exists());

                if (result.failed())
                {
                    expect (result.getErrorMessage().contains ("Close any program"));
                    expect (! store.findEntry (fixture.key())->isPlayable(), "song.json went first");
                }
            }

            expect (store.deleteEntry (fixture.key()).wasOk() || ! package.exists());
        }
    }

    //==========================================================================
    void plantAbandonedJob (const juce::File& jobs, const juce::String& id)
    {
        const auto job = jobs.getChildFile (id);
        job.getChildFile ("package").createDirectory();
        job.getChildFile ("package\\drums.wav").replaceWithText (juce::String::repeatedString ("a", 5000));
        job.getChildFile ("package\\bass.wav").replaceWithText (juce::String::repeatedString ("a", 5000));
        job.getChildFile ("package\\artwork.png").replaceWithText ("partial");
        job.getChildFile ("source-copy.tmp").replaceWithText ("scratch");
        job.getChildFile ("worker.log").replaceWithText ("the worker log");
        job.getChildFile ("worker-stderr.log").replaceWithText ("stderr");
        job.getChildFile ("artwork.png").replaceWithText ("staged artwork");
    }

    void runJobs()
    {
        beginTest ("Quarantine deletes abandoned audio and keeps logs and app files");
        {
            const test::ScopedTempDirectory temp;
            CacheStore store (temp.child ("cache"), temp.child ("jobs"));
            plantAbandonedJob (temp.child ("jobs"), "old1");
            plantAbandonedJob (temp.child ("jobs"), "old2");
            temp.child ("jobs\\logs-only").createDirectory();
            temp.child ("jobs\\logs-only\\worker.log").replaceWithText ("nothing to clean");

            const auto before = store.usage().jobBytes;
            const auto report = store.quarantineAbandonedJobs();

            expectEquals (report.foldersCleaned, 2);
            expectEquals (report.skippedActive, 0);
            expect (report.problems.isEmpty());
            expect (report.bytesFreed >= 20000);
            expect (store.usage().jobBytes == before - report.bytesFreed);

            for (const auto* id : { "old1", "old2" })
            {
                const auto job = temp.child ("jobs").getChildFile (id);
                expect (! job.getChildFile ("package").exists(), id);
                expect (! job.getChildFile ("source-copy.tmp").exists());
                expect (job.getChildFile ("worker.log").loadFileAsString() == "the worker log");
                expect (job.getChildFile ("worker-stderr.log").existsAsFile());
                expect (job.getChildFile ("artwork.png").existsAsFile());
            }

            const auto again = store.quarantineAbandonedJobs();
            expectEquals (again.foldersCleaned, 0);
            expect (again.bytesFreed == 0);
        }

        beginTest ("A running job's folder is never touched");
        {
            const test::ScopedTempDirectory temp;
            CacheStore store (temp.child ("cache"), temp.child ("jobs"));
            plantAbandonedJob (temp.child ("jobs"), "running");
            plantAbandonedJob (temp.child ("jobs"), "dead");

            {
                const auto token = store.markJobActive ("running");

                const auto jobs = store.listJobs();
                expect (jobs.size() == 2);
                expect (jobs[0].jobId == "dead" && ! jobs[0].active);
                expect (jobs[1].jobId == "running" && jobs[1].active);

                const auto report = store.quarantineAbandonedJobs();
                expectEquals (report.foldersCleaned, 1);
                expectEquals (report.skippedActive, 1);
                expect (temp.child ("jobs\\running\\package\\drums.wav").existsAsFile());
                expect (! temp.child ("jobs\\dead\\package").exists());

                expect (store.deleteJobFolder ("running").failed());
                expect (temp.child ("jobs\\running").isDirectory());
            }

            // The token ended with the job; the folder is abandoned now.
            expectEquals (store.quarantineAbandonedJobs().foldersCleaned, 1);
            expect (! temp.child ("jobs\\running\\package").exists());
            expect (temp.child ("jobs\\running\\worker.log").existsAsFile());
        }

        beginTest ("Active marks nest, move, and a whole job folder can be deleted once abandoned");
        {
            const test::ScopedTempDirectory temp;
            CacheStore store (temp.child ("cache"), temp.child ("jobs"));
            plantAbandonedJob (temp.child ("jobs"), "job");

            auto first = store.markJobActive ("job");
            auto second = store.markJobActive ("job");
            first = {};
            expect (store.listJobs()[0].active, "still held by the second mark");
            second = {};
            expect (! store.listJobs()[0].active);

            expect (store.deleteJobFolder ("..\\cache").failed());
            expect (store.deleteJobFolder ("job").wasOk());
            expect (! temp.child ("jobs\\job").exists());
            expect (store.deleteJobFolder ("job").wasOk(), "already gone is fine");
        }

        beginTest ("Job folders are never mistaken for songs");
        {
            const test::ScopedTempDirectory temp;
            CacheStore store (temp.child ("cache"), temp.child ("jobs"));
            plantAbandonedJob (temp.child ("jobs"), "job");
            expect (store.listEntries().empty());
            expect (store.listPlayableEntries().empty());
        }
    }
};

CacheStoreTests cacheStoreTests;
} // namespace
} // namespace snds
