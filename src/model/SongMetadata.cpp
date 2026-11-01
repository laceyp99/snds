#include "model/SongMetadata.h"

namespace snds
{
namespace
{
bool isInteger (const juce::var& value)
{
    return value.isInt() || value.isInt64();
}

bool isNumber (const juce::var& value)
{
    return isInteger (value) || value.isDouble();
}

bool isSha256 (const juce::String& text)
{
    return text.length() == 64 && text.containsOnly ("0123456789abcdef");
}

// YYYY-MM-DDTHH:MM:SS[.fraction]Z
bool isUtcTimestamp (const juce::String& text)
{
    if (text.length() < 20 || ! text.endsWithChar ('Z'))
        return false;

    for (int index = 0; index < 19; ++index)
    {
        const auto character = text[index];
        const bool separator = index == 4 || index == 7 || index == 10 || index == 13 || index == 16;
        const juce::juce_wchar expected = index == 4 || index == 7 ? '-' : index == 10 ? 'T' : ':';

        if (separator ? character != expected : ! juce::CharacterFunctions::isDigit (character))
            return false;
    }

    const auto fraction = text.substring (19, text.length() - 1);
    return fraction.isEmpty()
        || (fraction[0] == '.' && fraction.length() > 1 && fraction.substring (1).containsOnly ("0123456789"));
}

/** Reads the document, remembering the first problem. After a problem, reads return defaults. */
class Reader
{
public:
    const juce::DynamicObject* object (const juce::DynamicObject& parent, const char* key, const juce::String& path)
    {
        const auto value = parent.getProperty (key);

        if (auto* found = value.getDynamicObject())
            return found;

        fail (path + key + " is missing or not an object");
        return nullptr;
    }

    juce::String text (const juce::DynamicObject* parent, const char* key, const juce::String& path)
    {
        const auto value = valueOf (parent, key);

        if (value.isString() && value.toString().trim().isNotEmpty())
            return value.toString();

        fail (path + key + " must be a non-empty string");
        return {};
    }

    juce::String sha256 (const juce::DynamicObject* parent, const char* key, const juce::String& path)
    {
        const auto value = valueOf (parent, key).toString();

        if (isSha256 (value))
            return value;

        fail (path + key + " must be 64 lowercase hex characters");
        return {};
    }

    juce::int64 integer (const juce::DynamicObject* parent,
                         const char* key,
                         const juce::String& path,
                         juce::int64 minimum)
    {
        const auto value = valueOf (parent, key);

        if (isInteger (value) && (juce::int64) value >= minimum)
            return (juce::int64) value;

        fail (path + key + " must be an integer >= " + juce::String (minimum));
        return 0;
    }

    double number (
        const juce::DynamicObject* parent, const char* key, const juce::String& path, double minimum, double maximum)
    {
        const auto value = valueOf (parent, key);

        if (isNumber (value) && (double) value >= minimum && (double) value <= maximum)
            return (double) value;

        fail (path + key + " must be a number within range");
        return 0.0;
    }

    void fail (const juce::String& text)
    {
        if (problem.isEmpty())
            problem = text;
    }

    juce::String problem;

private:
    static juce::var valueOf (const juce::DynamicObject* parent, const char* key)
    {
        return parent != nullptr ? parent->getProperty (key) : juce::var();
    }
};

SongLoadResult makeResult (SongLoadResult::Status status, const juce::String& reason)
{
    SongLoadResult result;
    result.status = status;
    result.reason = reason;
    return result;
}

constexpr double largeValue = 1.0e18;
} // namespace

const std::array<const char*, SongMetadata::stemCount>& SongMetadata::stemNames()
{
    static const std::array<const char*, stemCount> names { "drums", "bass", "other", "vocals" };
    return names;
}

juce::String SongMetadata::cacheKey (const juce::String& sha256, juce::int64 sizeBytes)
{
    return sha256 + "-" + juce::String (sizeBytes);
}

juce::String SongLoadResult::statusName (Status status)
{
    switch (status)
    {
        case Status::complete:
            return "complete";
        case Status::incompatible:
            return "incompatible";
        case Status::corrupt:
            return "corrupt";
    }

    return {};
}

SongLoadResult parseSongMetadata (const juce::String& json, const SongCompatibility& compatibility)
{
    using Status = SongLoadResult::Status;

    juce::var parsed;

    if (juce::JSON::parse (json, parsed).failed())
        return makeResult (Status::corrupt, "song.json is not valid JSON");

    auto* root = parsed.getDynamicObject(); // not isObject(): that is also true for arrays

    if (root == nullptr)
        return makeResult (Status::corrupt, "song.json is not a JSON object");

    const auto schema = root->getProperty ("cacheSchema");

    if (! isInteger (schema))
        return makeResult (Status::corrupt, "cacheSchema is missing or not an integer");

    if ((juce::int64) schema != SongMetadata::cacheSchema)
        return makeResult (Status::incompatible,
                           "cacheSchema " + juce::String ((juce::int64) schema) + " is not supported (expected "
                               + juce::String (SongMetadata::cacheSchema) + ")");

    const auto status = root->getProperty ("status");

    if (! status.isString() || status.toString() != "complete")
        return makeResult (Status::corrupt, "status is " + status.toString().quoted() + ", not \"complete\"");

    SongMetadata song;
    Reader read;

    const auto* source = read.object (*root, "source", {});
    song.source.fileName = read.text (source, "fileName", "source.");
    song.source.path = read.text (source, "path", "source.");
    song.source.sizeBytes = read.integer (source, "sizeBytes", "source.", 0);
    song.source.sha256 = read.sha256 (source, "sha256", "source.");
    song.source.container = read.text (source, "container", "source.");
    song.source.sampleRate = (int) read.integer (source, "sampleRate", "source.", 1);
    song.source.channels = (int) read.integer (source, "channels", "source.", 1);
    song.source.frames = read.integer (source, "frames", "source.", 1);
    song.source.durationSeconds = read.number (source, "durationSeconds", "source.", 1.0e-9, largeValue);

    const auto* stems = read.object (*root, "stems", {});

    for (int index = 0; index < SongMetadata::stemCount; ++index)
    {
        const juce::String name (SongMetadata::stemNames()[(size_t) index]);
        const auto path = "stems." + name + ".";
        const auto* entry = stems != nullptr ? read.object (*stems, name.toRawUTF8(), "stems.") : nullptr;
        auto& stem = song.stems[(size_t) index];

        stem.file = read.text (entry, "file", path);
        stem.frames = read.integer (entry, "frames", path, 1);
        stem.sampleRate = (int) read.integer (entry, "sampleRate", path, 1);
        stem.channels = (int) read.integer (entry, "channels", path, 1);
        stem.sampleFormat = read.text (entry, "sampleFormat", path);
        stem.peak = read.number (entry, "peak", path, 0.0, largeValue);

        if (entry != nullptr && (entry->hasProperty ("sha256") || entry->hasProperty ("sizeBytes")))
        {
            stem.sha256 = read.sha256 (entry, "sha256", path);
            stem.sizeBytes = read.integer (entry, "sizeBytes", path, 1);
        }

        if (stem.file != name + ".wav")
            read.fail (path + "file must be " + name + ".wav");

        if (stem.channels != 2)
            read.fail (path + "channels must be 2");

        if (stem.sampleFormat != "float32")
            read.fail (path + "sampleFormat must be float32");

        if (stem.frames != song.source.frames)
            read.fail (path + "frames does not match source.frames");

        if (stem.sampleRate != song.source.sampleRate)
            read.fail (path + "sampleRate does not match source.sampleRate");
    }

    const auto* model = read.object (*root, "model", {});
    song.model.name = read.text (model, "name", "model.");
    song.model.signature = read.text (model, "signature", "model.");
    song.model.file = read.text (model, "file", "model.");
    song.model.sha256 = read.sha256 (model, "sha256", "model.");

    const auto* bpm = read.object (*root, "bpm", {});
    const bool bpmValueIsNull = bpm != nullptr && bpm->hasProperty ("value") && bpm->getProperty ("value").isVoid();

    if (bpmValueIsNull) // JSON null: detection failed, which does not fail the package
    {
        const auto error = bpm != nullptr ? bpm->getProperty ("error") : juce::var();
        song.bpm.error =
            error.isString() && error.toString().trim().isNotEmpty() ? error.toString() : juce::String ("not detected");
    }
    else
    {
        song.bpm.value = read.number (bpm, "value", "bpm.", 1.0e-9, largeValue);
    }

    song.bpm.confidence = read.number (bpm, "confidence", "bpm.", 0.0, 1.0);
    song.bpm.beats = (int) read.integer (bpm, "beats", "bpm.", 0);

    const auto alternatives = bpm != nullptr ? bpm->getProperty ("alternatives") : juce::var();

    if (auto* list = alternatives.getArray())
    {
        for (const auto& entry : *list)
        {
            if (isNumber (entry) && (double) entry > 0.0)
                song.bpm.alternatives.add ((double) entry);
            else
                read.fail ("bpm.alternatives must be a list of positive numbers");
        }
    }
    else
    {
        read.fail ("bpm.alternatives must be a list of positive numbers");
    }

    const auto* analyzer = bpm != nullptr ? read.object (*bpm, "analyzer", "bpm.") : nullptr;
    song.bpm.analyzerName = read.text (analyzer, "name", "bpm.analyzer.");
    song.bpm.analyzerVersion = read.text (analyzer, "version", "bpm.analyzer.");
    song.bpm.checkpointSha256 = read.sha256 (analyzer, "checkpointSha256", "bpm.analyzer.");

    const auto override = bpm != nullptr ? bpm->getProperty ("userOverride") : juce::var();

    if (auto* overrideObject = override.getDynamicObject())
    {
        SongMetadata::BpmOverride entry;
        entry.value = read.number (overrideObject, "value", "bpm.userOverride.", 1.0e-9, largeValue);
        entry.mode = read.text (overrideObject, "mode", "bpm.userOverride.");

        if (entry.mode != "numeric" && entry.mode != "half" && entry.mode != "double")
            read.fail ("bpm.userOverride.mode must be numeric, half or double");

        song.bpm.userOverride = entry;
    }
    else if (! override.isVoid())
    {
        read.fail ("bpm.userOverride must be an object or null");
    }

    const auto* artwork = read.object (*root, "artwork", {});
    const auto artworkFile = artwork != nullptr ? artwork->getProperty ("file") : juce::var();

    if (artworkFile.isString() && artworkFile.toString().trim().isNotEmpty())
    {
        song.artwork.file = artworkFile.toString();

        if (song.artwork.file.containsAnyOf ("/\\:"))
            read.fail ("artwork.file must be a plain file name");
    }
    else if (! artworkFile.isVoid())
        read.fail ("artwork.file must be a string or null");

    song.artwork.origin = read.text (artwork, "origin", "artwork.");

    if (song.artwork.origin != "embedded" && song.artwork.origin != "file" && song.artwork.origin != "fallback")
        read.fail ("artwork.origin must be embedded, file or fallback");

    const auto* worker = read.object (*root, "worker", {});
    song.worker.version = read.text (worker, "version", "worker.");
    song.worker.protocol = (int) read.integer (worker, "protocol", "worker.", 0);
    song.worker.torch = read.text (worker, "torch", "worker.");
    song.worker.device = read.text (worker, "device", "worker.");

    const auto preparedAt = root->getProperty ("preparedAt").toString();

    if (isUtcTimestamp (preparedAt))
        song.preparedAt = preparedAt;
    else
        read.fail ("preparedAt must be a UTC ISO-8601 timestamp ending in Z");

    if (read.problem.isNotEmpty())
        return makeResult (Status::corrupt, read.problem);

    if (song.model.name != compatibility.modelName || song.model.signature != compatibility.modelSignature)
        return makeResult (Status::incompatible, "prepared with model " + song.model.name + " " + song.model.signature);

    if (song.worker.protocol != compatibility.protocol)
        return makeResult (Status::incompatible, "prepared with protocol " + juce::String (song.worker.protocol));

    auto result = makeResult (Status::complete, {});
    result.metadata = std::move (song);
    return result;
}

SongLoadResult loadSongPackage (const juce::File& packageDir, const SongCompatibility& compatibility)
{
    using Status = SongLoadResult::Status;

    const auto songFile = packageDir.getChildFile ("song.json");

    if (! songFile.existsAsFile())
        return makeResult (Status::corrupt, "song.json is missing");

    auto result = parseSongMetadata (songFile.loadFileAsString(), compatibility);

    if (result.status == Status::corrupt)
        return result;

    // Compatibility is judged on the document alone; only a complete one has the fields below.
    if (! result.isComplete())
        return result;

    const auto& song = result.metadata;

    if (packageDir.getFileName() != song.cacheKey())
        return makeResult (Status::corrupt, "folder name does not match the cache key " + song.cacheKey());

    for (const auto& stem : song.stems)
        if (! packageDir.getChildFile (stem.file).existsAsFile())
            return makeResult (Status::corrupt, stem.file + " is missing");

    if (song.artwork.file.isNotEmpty() && ! packageDir.getChildFile (song.artwork.file).existsAsFile())
        return makeResult (Status::corrupt, song.artwork.file + " is missing");

    return result;
}
} // namespace snds
