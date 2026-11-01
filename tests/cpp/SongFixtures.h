#pragma once

#include <juce_core/juce_core.h>

#include "model/SongMetadata.h"

#include <functional>

namespace snds::test
{
/** Builds cache packages on disk for tests: a song.json that satisfies docs/protocol.md plus
    placeholder stem files (the app checks that stems exist, the worker validates their audio). */
struct SongFixture
{
    juce::String sha256 = juce::String::repeatedString ("ab", 32);
    juce::int64 sizeBytes = 1234;
    juce::String preparedAt = "2026-10-04T12:00:00Z";

    juce::String key() const { return SongMetadata::cacheKey (sha256, sizeBytes); }

    juce::var document() const
    {
        auto* source = new juce::DynamicObject();
        source->setProperty ("fileName", "song.wav");
        source->setProperty ("path", "C:\\Music\\song.wav");
        source->setProperty ("sizeBytes", sizeBytes);
        source->setProperty ("sha256", sha256);
        source->setProperty ("container", "wav");
        source->setProperty ("sampleRate", 44100);
        source->setProperty ("channels", 2);
        source->setProperty ("frames", (juce::int64) 441000);
        source->setProperty ("durationSeconds", 10.0);

        auto* stems = new juce::DynamicObject();

        for (const auto* name : SongMetadata::stemNames())
        {
            auto* stem = new juce::DynamicObject();
            stem->setProperty ("file", juce::String (name) + ".wav");
            stem->setProperty ("frames", (juce::int64) 441000);
            stem->setProperty ("sampleRate", 44100);
            stem->setProperty ("channels", 2);
            stem->setProperty ("sampleFormat", "float32");
            stem->setProperty ("peak", 0.9);
            stems->setProperty (name, juce::var (stem));
        }

        auto* model = new juce::DynamicObject();
        model->setProperty ("name", "htdemucs");
        model->setProperty ("signature", "955717e8");
        model->setProperty ("file", "955717e8-8726e21a.th");
        model->setProperty ("sha256", juce::String::repeatedString ("cd", 32));

        auto* analyzer = new juce::DynamicObject();
        analyzer->setProperty ("name", "beat_this");
        analyzer->setProperty ("version", "1.1.0");
        analyzer->setProperty ("checkpointSha256", juce::String::repeatedString ("cd", 32));

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
        root->setProperty ("preparedAt", preparedAt);
        return juce::var (root);
    }

    /** Writes `<cacheDir>\<key>` with song.json (after `edit` changed the document) and four
        stem files of `stemBytes` bytes each. Returns the package folder. */
    juce::File write (const juce::File& cacheDir,
                      const std::function<void (juce::DynamicObject&)>& edit = {},
                      int stemBytes = 2000) const
    {
        const auto package = cacheDir.getChildFile (key());
        package.createDirectory();

        for (const auto* name : SongMetadata::stemNames())
            package.getChildFile (juce::String (name) + ".wav")
                .replaceWithText (juce::String::repeatedString ("x", stemBytes));

        auto doc = document();

        if (edit)
            edit (*doc.getDynamicObject());

        package.getChildFile ("song.json").replaceWithText (juce::JSON::toString (doc));
        return package;
    }
};
} // namespace snds::test
