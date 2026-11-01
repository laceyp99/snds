#include <juce_core/juce_core.h>

#include "TestHelpers.h"
#include "model/SongMetadata.h"

#include <functional>

namespace snds
{
namespace
{
using Status = SongLoadResult::Status;

const juce::String sourceSha = juce::String::repeatedString ("ab", 32);
const juce::String otherSha = juce::String::repeatedString ("cd", 32);

juce::var stemEntry (const juce::String& name)
{
    auto* entry = new juce::DynamicObject();
    entry->setProperty ("file", name + ".wav");
    entry->setProperty ("frames", (juce::int64) 441000);
    entry->setProperty ("sampleRate", 44100);
    entry->setProperty ("channels", 2);
    entry->setProperty ("sampleFormat", "float32");
    entry->setProperty ("peak", 0.9);
    return juce::var (entry);
}

juce::var goodDocument()
{
    auto* source = new juce::DynamicObject();
    source->setProperty ("fileName", "song.wav");
    source->setProperty ("path", "C:\\Music\\song.wav");
    source->setProperty ("sizeBytes", (juce::int64) 1234);
    source->setProperty ("sha256", sourceSha);
    source->setProperty ("container", "wav");
    source->setProperty ("sampleRate", 44100);
    source->setProperty ("channels", 2);
    source->setProperty ("frames", (juce::int64) 441000);
    source->setProperty ("durationSeconds", 10.0);

    auto* stems = new juce::DynamicObject();

    for (const auto* name : SongMetadata::stemNames())
        stems->setProperty (name, stemEntry (name));

    auto* model = new juce::DynamicObject();
    model->setProperty ("name", "htdemucs");
    model->setProperty ("signature", "955717e8");
    model->setProperty ("file", "955717e8-8726e21a.th");
    model->setProperty ("sha256", otherSha);

    auto* analyzer = new juce::DynamicObject();
    analyzer->setProperty ("name", "beat_this");
    analyzer->setProperty ("version", "1.1.0");
    analyzer->setProperty ("checkpointSha256", otherSha);

    auto* bpm = new juce::DynamicObject();
    bpm->setProperty ("value", 120.0);
    bpm->setProperty ("confidence", 0.8);
    bpm->setProperty ("beats", 20);
    bpm->setProperty ("alternatives", juce::Array<juce::var> { 60.0, 240.0 });
    bpm->setProperty ("analyzer", juce::var (analyzer));
    bpm->setProperty ("userOverride", juce::var());

    auto* artwork = new juce::DynamicObject();
    artwork->setProperty ("file", juce::var());
    artwork->setProperty ("origin", "fallback");

    auto* worker = new juce::DynamicObject();
    worker->setProperty ("version", "0.0.1");
    worker->setProperty ("protocol", 1);
    worker->setProperty ("torch", "2.1.2+cpu");
    worker->setProperty ("device", "cpu");

    auto* root = new juce::DynamicObject();
    root->setProperty ("cacheSchema", 1);
    root->setProperty ("status", "complete");
    root->setProperty ("source", juce::var (source));
    root->setProperty ("stems", juce::var (stems));
    root->setProperty ("model", juce::var (model));
    root->setProperty ("bpm", juce::var (bpm));
    root->setProperty ("artwork", juce::var (artwork));
    root->setProperty ("worker", juce::var (worker));
    root->setProperty ("preparedAt", "2026-10-04T12:00:00Z");
    return juce::var (root);
}

juce::String text (const juce::var& document)
{
    return juce::JSON::toString (document, false, 20);
}

class SongMetadataTests final : public juce::UnitTest
{
public:
    SongMetadataTests()
        : juce::UnitTest ("SongMetadata", "snds")
    {
    }

    void runTest() override
    {
        beginTest ("Setup");
        parsesCompleteDocument();
        classifiesSchemaAndStatus();
        flagsStructuralProblems();
        flagsIncompatibility();
        loadsPackages();
    }

private:
    SongLoadResult parseWith (const std::function<void (juce::DynamicObject&)>& mutate)
    {
        auto document = goodDocument();
        mutate (*document.getDynamicObject());
        return parseSongMetadata (text (document));
    }

    void expectStatus (const SongLoadResult& result, Status expected, const juce::String& description)
    {
        expect (result.status == expected,
                description + ": expected " + SongLoadResult::statusName (expected) + " but got "
                    + SongLoadResult::statusName (result.status) + " (" + result.reason + ")");
        expect (expected == Status::complete || result.reason.isNotEmpty(),
                description + ": reason should not be empty");
    }

    void parsesCompleteDocument()
    {
        beginTest ("a complete document loads every field");
        const auto result = parseSongMetadata (text (goodDocument()));
        expectStatus (result, Status::complete, "good document");

        if (! result.isComplete())
            return;

        const auto& song = result.metadata;
        expectEquals (song.source.fileName, juce::String ("song.wav"));
        expect (song.source.sizeBytes == 1234);
        expectEquals (song.source.sampleRate, 44100);
        expect (song.source.frames == 441000);
        expectEquals (song.cacheKey(), sourceSha + "-1234");
        expectEquals (song.stems[0].file, juce::String ("drums.wav"));
        expectEquals (song.stems[3].file, juce::String ("vocals.wav"));
        expectWithinAbsoluteError (song.stems[1].peak, 0.9, 1.0e-9);
        expectEquals (song.model.signature, juce::String ("955717e8"));
        expectEquals (song.bpm.alternatives.size(), 2);
        expectEquals (song.bpm.analyzerName, juce::String ("beat_this"));
        expect (! song.bpm.userOverride.has_value());
        expectWithinAbsoluteError (song.effectiveBpm(), 120.0, 1.0e-9);
        expect (song.artwork.file.isEmpty());
        expectEquals (song.artwork.origin, juce::String ("fallback"));
        expectEquals (song.worker.device, juce::String ("cpu"));
        expectEquals (song.preparedAt, juce::String ("2026-10-04T12:00:00Z"));

        beginTest ("artwork file and a user BPM override are read");
        const auto overridden = parseWith (
            [] (juce::DynamicObject& root)
            {
                root.getProperty ("artwork").getDynamicObject()->setProperty ("file", "artwork.png");
                root.getProperty ("artwork").getDynamicObject()->setProperty ("origin", "embedded");
                auto* override = new juce::DynamicObject();
                override->setProperty ("value", 60.0);
                override->setProperty ("mode", "half");
                root.getProperty ("bpm").getDynamicObject()->setProperty ("userOverride", juce::var (override));
            });
        expectStatus (overridden, Status::complete, "overridden document");

        if (overridden.isComplete())
        {
            expectEquals (overridden.metadata.artwork.file, juce::String ("artwork.png"));
            expectWithinAbsoluteError (overridden.metadata.effectiveBpm(), 60.0, 1.0e-9);
            expectEquals (overridden.metadata.bpm.userOverride->mode, juce::String ("half"));
        }

        beginTest ("a null BPM (detection failed) still loads and falls back to the default");
        const auto undetected = parseWith (
            [] (juce::DynamicObject& root)
            {
                auto* bpmObject = root.getProperty ("bpm").getDynamicObject();
                bpmObject->setProperty ("value", juce::var());
                bpmObject->setProperty ("confidence", 0.0);
                bpmObject->setProperty ("alternatives", juce::Array<juce::var>());
                bpmObject->setProperty ("error", "no beats found");
            });
        expectStatus (undetected, Status::complete, "null bpm");

        if (undetected.isComplete())
        {
            expect (! undetected.metadata.bpm.value.has_value());
            expect (! undetected.metadata.hasKnownBpm());
            expectEquals (undetected.metadata.bpm.error, juce::String ("no beats found"));
            expectWithinAbsoluteError (undetected.metadata.effectiveBpm(), SongMetadata::defaultBpm, 1.0e-9);
        }

        expectStatus (parseWith ([] (juce::DynamicObject& root)
                                 { root.getProperty ("bpm").getDynamicObject()->removeProperty ("value"); }),
                      Status::corrupt,
                      "bpm.value missing entirely");

        beginTest ("non-ASCII source names survive");
        const auto unicode = parseWith (
            [] (juce::DynamicObject& root)
            {
                root.getProperty ("source").getDynamicObject()->setProperty (
                    "fileName", juce::String (juce::CharPointer_UTF8 ("\xe6\x9b\xb2 \xf0\x9f\x8e\xb5.wav")));
            });
        expect (unicode.isComplete());

        if (unicode.isComplete())
            expectEquals (unicode.metadata.source.fileName,
                          juce::String (juce::CharPointer_UTF8 ("\xe6\x9b\xb2 \xf0\x9f\x8e\xb5.wav")));
    }

    void classifiesSchemaAndStatus()
    {
        beginTest ("unparseable and non-object documents are corrupt");

        for (const auto* bad : { "", "not json", "{\"cacheSchema\":", "[]", "[1,2]", "\"x\"", "42", "null", "{}" })
            expectStatus (parseSongMetadata (bad), Status::corrupt, juce::String ("document ") + bad);

        beginTest ("another cacheSchema is incompatible, an unreadable one is corrupt");

        for (const int schema : { 0, 2, 99 })
            expectStatus (
                parseWith ([schema] (juce::DynamicObject& root) { root.setProperty ("cacheSchema", schema); }),
                Status::incompatible,
                "cacheSchema " + juce::String (schema));

        expectStatus (parseWith ([] (juce::DynamicObject& root) { root.setProperty ("cacheSchema", "1"); }),
                      Status::corrupt,
                      "string schema");
        expectStatus (parseWith ([] (juce::DynamicObject& root) { root.setProperty ("cacheSchema", 1.5); }),
                      Status::corrupt,
                      "fractional schema");
        expectStatus (parseWith ([] (juce::DynamicObject& root) { root.removeProperty ("cacheSchema"); }),
                      Status::corrupt,
                      "missing schema");

        beginTest ("status other than complete is corrupt");

        for (const auto* status : { "preparing", "", "Complete" })
            expectStatus (parseWith ([status] (juce::DynamicObject& root) { root.setProperty ("status", status); }),
                          Status::corrupt,
                          juce::String ("status ") + status);

        expectStatus (parseWith ([] (juce::DynamicObject& root) { root.removeProperty ("status"); }),
                      Status::corrupt,
                      "missing status");
    }

    void flagsStructuralProblems()
    {
        beginTest ("a missing stem is corrupt and named");

        for (const auto* stemName : SongMetadata::stemNames())
        {
            const auto result =
                parseWith ([stemName] (juce::DynamicObject& root)
                           { root.getProperty ("stems").getDynamicObject()->removeProperty (stemName); });
            expectStatus (result, Status::corrupt, juce::String ("missing ") + stemName);
            expect (result.reason.contains (stemName), result.reason);
        }

        beginTest ("stem contract violations are corrupt");
        expectStatus (parseWith (
                          [] (juce::DynamicObject& root)
                          {
                              root.getProperty ("stems")["bass"].getDynamicObject()->setProperty ("frames",
                                                                                                  (juce::int64) 440999);
                          }),
                      Status::corrupt,
                      "frame mismatch");
        expectStatus (
            parseWith ([] (juce::DynamicObject& root)
                       { root.getProperty ("stems")["bass"].getDynamicObject()->setProperty ("channels", 1); }),
            Status::corrupt,
            "mono stem");
        expectStatus (
            parseWith (
                [] (juce::DynamicObject& root)
                { root.getProperty ("stems")["bass"].getDynamicObject()->setProperty ("sampleFormat", "int16"); }),
            Status::corrupt,
            "int16 stem");
        expectStatus (
            parseWith ([] (juce::DynamicObject& root)
                       { root.getProperty ("stems")["drums"].getDynamicObject()->setProperty ("sampleRate", 48000); }),
            Status::corrupt,
            "sample rate mismatch");
        expectStatus (
            parseWith (
                [] (juce::DynamicObject& root)
                { root.getProperty ("stems")["drums"].getDynamicObject()->setProperty ("file", "..\\drums.wav"); }),
            Status::corrupt,
            "stem path escape");

        beginTest ("other malformed fields are corrupt");
        expectStatus (parseWith ([] (juce::DynamicObject& root) { root.removeProperty ("source"); }),
                      Status::corrupt,
                      "missing source");
        expectStatus (parseWith ([] (juce::DynamicObject& root)
                                 { root.setProperty ("source", juce::Array<juce::var> { 1, 2 }); }),
                      Status::corrupt,
                      "source is an array");
        expectStatus (parseWith ([] (juce::DynamicObject& root)
                                 { root.getProperty ("source").getDynamicObject()->setProperty ("sha256", "XYZ"); }),
                      Status::corrupt,
                      "short sha");
        expectStatus (
            parseWith (
                [] (juce::DynamicObject& root)
                { root.getProperty ("source").getDynamicObject()->setProperty ("sha256", sourceSha.toUpperCase()); }),
            Status::corrupt,
            "uppercase sha");
        expectStatus (parseWith ([] (juce::DynamicObject& root)
                                 { root.getProperty ("bpm").getDynamicObject()->setProperty ("value", 0.0); }),
                      Status::corrupt,
                      "zero bpm");
        expectStatus (parseWith ([] (juce::DynamicObject& root)
                                 { root.getProperty ("bpm").getDynamicObject()->setProperty ("confidence", 1.5); }),
                      Status::corrupt,
                      "confidence above one");
        expectStatus (parseWith ([] (juce::DynamicObject& root)
                                 { root.getProperty ("bpm").getDynamicObject()->setProperty ("alternatives", "60"); }),
                      Status::corrupt,
                      "alternatives not a list");
        expectStatus (parseWith (
                          [] (juce::DynamicObject& root)
                          {
                              auto* override = new juce::DynamicObject();
                              override->setProperty ("value", 70.0);
                              override->setProperty ("mode", "triple");
                              root.getProperty ("bpm").getDynamicObject()->setProperty ("userOverride",
                                                                                        juce::var (override));
                          }),
                      Status::corrupt,
                      "bad override mode");
        expectStatus (parseWith ([] (juce::DynamicObject& root)
                                 { root.getProperty ("artwork").getDynamicObject()->setProperty ("origin", "web"); }),
                      Status::corrupt,
                      "bad artwork origin");
        expectStatus (
            parseWith ([] (juce::DynamicObject& root)
                       { root.getProperty ("artwork").getDynamicObject()->setProperty ("file", "..\\x.png"); }),
            Status::corrupt,
            "artwork path escape");

        for (const auto* stamp : { "yesterday", "2026-10-04T12:00:00+02:00", "2026-10-04 12:00:00Z", "" })
            expectStatus (parseWith ([stamp] (juce::DynamicObject& root) { root.setProperty ("preparedAt", stamp); }),
                          Status::corrupt,
                          juce::String ("preparedAt ") + stamp);

        expect (
            parseWith ([] (juce::DynamicObject& root) { root.setProperty ("preparedAt", "2026-10-04T12:00:00.250Z"); })
                .isComplete());
    }

    void flagsIncompatibility()
    {
        beginTest ("another model or protocol is incompatible");
        expectStatus (
            parseWith ([] (juce::DynamicObject& root)
                       { root.getProperty ("model").getDynamicObject()->setProperty ("signature", "deadbeef"); }),
            Status::incompatible,
            "other signature");
        expectStatus (parseWith ([] (juce::DynamicObject& root)
                                 { root.getProperty ("model").getDynamicObject()->setProperty ("name", "mdx"); }),
                      Status::incompatible,
                      "other model");
        expectStatus (parseWith ([] (juce::DynamicObject& root)
                                 { root.getProperty ("worker").getDynamicObject()->setProperty ("protocol", 2); }),
                      Status::incompatible,
                      "other protocol");

        beginTest ("compatibility comes from the caller");
        SongCompatibility future;
        future.modelSignature = "12345678";
        expectStatus (parseSongMetadata (text (goodDocument()), future), Status::incompatible, "newer build");
    }

    void loadsPackages()
    {
        const test::ScopedTempDirectory temp;
        const auto key = sourceSha + "-1234";

        const auto makePackage = [&] (const juce::String& folderName)
        {
            const auto folder = temp.child (folderName);
            folder.createDirectory();
            folder.getChildFile ("song.json").replaceWithText (text (goodDocument()));

            for (const auto* stemName : SongMetadata::stemNames())
                folder.getChildFile (juce::String (stemName) + ".wav").replaceWithText ("RIFF");

            return folder;
        };

        beginTest ("a package with song.json and four stems loads");
        expectStatus (loadSongPackage (makePackage (key)), Status::complete, "good package");

        beginTest ("a folder without song.json is corrupt");
        const auto empty = temp.child ("empty");
        empty.createDirectory();
        expectStatus (loadSongPackage (empty), Status::corrupt, "no song.json");
        expectStatus (loadSongPackage (temp.child ("does-not-exist")), Status::corrupt, "no folder");

        beginTest ("a folder named differently from the cache key is corrupt");
        expectStatus (loadSongPackage (makePackage ("renamed")), Status::corrupt, "wrong folder name");

        beginTest ("a missing stem file is corrupt");
        const auto folder = temp.child (key);
        folder.getChildFile ("vocals.wav").deleteFile();
        const auto missing = loadSongPackage (folder);
        expectStatus (missing, Status::corrupt, "missing stem file");
        expect (missing.reason.contains ("vocals.wav"), missing.reason);

        beginTest ("a named artwork file must exist");
        {
            const test::ScopedTempDirectory other;
            const auto package = other.child (key);
            package.createDirectory();
            auto document = goodDocument();
            document.getProperty ("artwork", {}).getDynamicObject()->setProperty ("file", "artwork.png");
            document.getProperty ("artwork", {}).getDynamicObject()->setProperty ("origin", "file");
            package.getChildFile ("song.json").replaceWithText (text (document));

            for (const auto* stemName : SongMetadata::stemNames())
                package.getChildFile (juce::String (stemName) + ".wav").replaceWithText ("RIFF");

            expectStatus (loadSongPackage (package), Status::corrupt, "artwork missing");
            package.getChildFile ("artwork.png").replaceWithText ("png");
            expectStatus (loadSongPackage (package), Status::complete, "artwork present");
        }

        beginTest ("a garbled song.json is corrupt");
        folder.getChildFile ("song.json").replaceWithText ("{ \"cacheSchema\": 1, ");
        expectStatus (loadSongPackage (folder), Status::corrupt, "truncated song.json");

        beginTest ("an old schema is incompatible even if the files are missing");
        folder.getChildFile ("song.json").replaceWithText (R"({"cacheSchema":0,"status":"complete"})");
        expectStatus (loadSongPackage (folder), Status::incompatible, "old schema");
    }
};

const SongMetadataTests songMetadataTests;
} // namespace
} // namespace snds
