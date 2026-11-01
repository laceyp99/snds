#include "app/Settings.h"

namespace snds
{
namespace
{
const juce::Identifier formatKey ("format");
const juce::Identifier versionKey ("version");
const juce::Identifier forceCpuKey ("forceCpu");
const juce::Identifier audioDeviceKey ("audioDevice");
const juce::Identifier recentSongsKey ("recentSongs");
} // namespace

Settings::Settings (juce::File settingsFile)
    : file (std::move (settingsFile)),
      values (new juce::DynamicObject())
{
}

Settings::LoadResult Settings::load()
{
    values = new juce::DynamicObject();
    backupBeforeSave = false;

    if (! file.exists())
        return {};

    const auto unusable = [this] (LoadStatus status, const juce::String& problem)
    {
        backupBeforeSave = true;
        return LoadResult {
            status, { problem + " Using default settings; the file will be backed up before SNDS saves settings." }
        };
    };

    juce::FileInputStream input (file);

    if (! input.openedOk())
        return unusable (LoadStatus::unreadable,
                         "Could not read " + file.getFullPathName() + " (" + input.getStatus().getErrorMessage()
                             + ").");

    juce::var parsed;
    const auto parseResult = juce::JSON::parse (input.readEntireStreamAsString(), parsed);

    // getDynamicObject() rather than isObject(): JUCE reports JSON arrays as objects too.
    auto* object = parseResult.wasOk() ? parsed.getDynamicObject() : nullptr;

    if (object == nullptr)
        return unusable (LoadStatus::invalid, file.getFullPathName() + " does not contain a JSON object.");

    if (object->getProperty (formatKey).toString() != formatName)
        return unusable (LoadStatus::foreign, file.getFullPathName() + " was not written by SNDS.");

    const auto version = object->getProperty (versionKey);

    if (! (version.isInt() || version.isInt64()) || static_cast<int> (version) < 1)
        return unusable (LoadStatus::invalid, file.getFullPathName() + " has no valid format version.");

    if (static_cast<int> (version) > formatVersion)
        return unusable (LoadStatus::newerVersion,
                         file.getFullPathName() + " uses settings format " + version.toString()
                             + ", which is newer than this SNDS understands.");

    LoadResult result { LoadStatus::loaded, {} };
    values = object;

    if (values->hasProperty (forceCpuKey) && ! values->getProperty (forceCpuKey).isBool())
    {
        values->removeProperty (forceCpuKey);
        result.warnings.add ("Ignored an invalid \"forceCpu\" value in " + file.getFullPathName() + ".");
    }

    if (values->hasProperty (audioDeviceKey) && ! values->getProperty (audioDeviceKey).isString())
    {
        values->removeProperty (audioDeviceKey);
        result.warnings.add ("Ignored an invalid \"audioDevice\" value in " + file.getFullPathName() + ".");
    }

    if (values->hasProperty (recentSongsKey) && ! values->getProperty (recentSongsKey).isArray())
    {
        values->removeProperty (recentSongsKey);
        result.warnings.add ("Ignored an invalid \"recentSongs\" value in " + file.getFullPathName() + ".");
    }

    return result;
}

juce::Result Settings::save()
{
    lastBackup = juce::File();

    if (backupBeforeSave && file.existsAsFile())
    {
        auto backup = file.getSiblingFile (file.getFileName() + ".bak");

        if (backup.exists())
            backup = backup.getNonexistentSibling();

        if (! file.copyFileTo (backup))
            return juce::Result::fail ("Could not back up " + file.getFullPathName() + " to " + backup.getFullPathName()
                                       + ".");

        lastBackup = backup;
    }

    juce::DynamicObject::Ptr output (new juce::DynamicObject());
    output->setProperty (formatKey, formatName);
    output->setProperty (versionKey, formatVersion);

    for (const auto& property : values->getProperties())
        if (property.name != formatKey && property.name != versionKey)
            output->setProperty (property.name, property.value);

    if (const auto created = file.getParentDirectory().createDirectory(); created.failed())
        return created;

    const juce::TemporaryFile temporary (file);

    if (! temporary.getFile().replaceWithText (juce::JSON::toString (juce::var (output.get())) + "\n"))
        return juce::Result::fail ("Could not write " + temporary.getFile().getFullPathName() + ".");

    if (! temporary.overwriteTargetFileWithTemporary())
        return juce::Result::fail ("Could not replace " + file.getFullPathName() + ".");

    backupBeforeSave = false;
    return juce::Result::ok();
}

bool Settings::getForceCpu() const
{
    return values->getProperty (forceCpuKey);
}

void Settings::setForceCpu (bool shouldForceCpu)
{
    values->setProperty (forceCpuKey, shouldForceCpu);
}

juce::String Settings::getAudioDeviceState() const
{
    return values->getProperty (audioDeviceKey).toString();
}

void Settings::setAudioDeviceState (const juce::String& stateXml)
{
    if (stateXml.isEmpty())
        values->removeProperty (audioDeviceKey);
    else
        values->setProperty (audioDeviceKey, stateXml);
}

std::vector<RecentSong> Settings::getRecentSongs() const
{
    std::vector<RecentSong> songs;
    const auto list = values->getProperty (recentSongsKey);

    if (const auto* array = list.getArray())
    {
        for (const auto& item : *array)
        {
            const auto key = item.getProperty ("key", {}).toString();

            // The key names a folder in the cache: accept only a plain name.
            if (key.isEmpty()
                || ! key.containsOnly ("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_"))
                continue;

            songs.push_back (
                { key, item.getProperty ("title", {}).toString(), item.getProperty ("path", {}).toString() });

            if (songs.size() == recent::maxEntries)
                break;
        }
    }

    return songs;
}

void Settings::setRecentSongs (const std::vector<RecentSong>& songs)
{
    juce::Array<juce::var> array;

    for (const auto& song : songs)
    {
        auto* object = new juce::DynamicObject();
        object->setProperty ("key", song.key);
        object->setProperty ("title", song.title);
        object->setProperty ("path", song.sourcePath);
        array.add (juce::var (object));
    }

    values->setProperty (recentSongsKey, array);
}
} // namespace snds
