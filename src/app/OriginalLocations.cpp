#include "app/OriginalLocations.h"

namespace snds
{
OriginalLocations::OriginalLocations (juce::File storageFile)
    : file (std::move (storageFile))
{
    load();
}

void OriginalLocations::load()
{
    juce::var parsed;

    if (! file.existsAsFile() || juce::JSON::parse (file.loadFileAsString(), parsed).failed())
        return;

    auto* root = parsed.getDynamicObject();

    if (root == nullptr || static_cast<int> (root->getProperty ("version")) != formatVersion)
        return;

    if (auto* originals = root->getProperty ("originals").getDynamicObject())
        for (const auto& property : originals->getProperties())
            if (property.value.isString())
                locations.set (property.name.toString(), property.value.toString());
}

std::optional<juce::File> OriginalLocations::find (const juce::String& cacheKey) const
{
    if (! locations.containsKey (cacheKey))
        return std::nullopt;

    return juce::File (locations[cacheKey]);
}

bool OriginalLocations::remember (const juce::String& cacheKey, const juce::File& original)
{
    locations.set (cacheKey, original.getFullPathName());

    auto* originals = new juce::DynamicObject();

    for (const auto& key : locations.getAllKeys())
        originals->setProperty (juce::Identifier (key), locations[key]);

    auto* root = new juce::DynamicObject();
    root->setProperty ("version", formatVersion);
    root->setProperty ("originals", juce::var (originals));

    if (! file.getParentDirectory().createDirectory().wasOk())
        return false;

    juce::TemporaryFile temporary (file);

    return temporary.getFile().replaceWithText (juce::JSON::toString (juce::var (root)), false, false, "\r\n")
        && temporary.overwriteTargetFileWithTemporary();
}
} // namespace snds
