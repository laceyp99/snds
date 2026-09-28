#pragma once

#include <juce_core/juce_core.h>

namespace snds::test
{

/** Category shared by every SNDS unit test; the runner only runs this category. */
inline constexpr const char* testCategory = "SNDS";

/** A unique directory below the system temp folder, deleted recursively on destruction. */
class ScopedTempDirectory
{
public:
    ScopedTempDirectory()
        : directory (juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getNonexistentChildFile (
                             "snds-test-" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()),
                             {},
                             false))
    {
        directory.createDirectory();
    }

    ~ScopedTempDirectory() { directory.deleteRecursively(); }

    const juce::File& get() const noexcept { return directory; }

    /** Creates a file (and its parents) with the given text, returning it. */
    juce::File createFile (const juce::String& relativePath, const juce::String& content = {}) const
    {
        const auto file = directory.getChildFile (relativePath);
        file.getParentDirectory().createDirectory();
        file.replaceWithText (content);
        return file;
    }

private:
    juce::File directory;

    JUCE_DECLARE_NON_COPYABLE (ScopedTempDirectory)
};

} // namespace snds::test
