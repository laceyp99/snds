#include "app/SongPackageEdit.h"

namespace snds
{
namespace
{
/** Where the value of "userOverride" starts and ends in the document text, or false. */
bool findOverrideValue (const juce::String& text, int& start, int& end)
{
    const auto key = text.indexOf ("\"userOverride\"");

    if (key < 0 || text.indexOf (key + 1, "\"userOverride\"") >= 0)
        return false; // missing, or ambiguous

    auto cursor = key + 14;

    while (cursor < text.length() && juce::CharacterFunctions::isWhitespace (text[cursor]))
        ++cursor;

    if (cursor >= text.length() || text[cursor] != ':')
        return false;

    ++cursor;

    while (cursor < text.length() && juce::CharacterFunctions::isWhitespace (text[cursor]))
        ++cursor;

    start = cursor;

    if (text.substring (cursor, cursor + 4) == "null")
    {
        end = cursor + 4;
        return true;
    }

    if (text[cursor] != '{')
        return false;

    const auto close = text.indexOfChar (cursor, '}');

    if (close < 0 || text.substring (cursor + 1, close).containsChar ('{'))
        return false; // the override is a flat object

    end = close + 1;
    return true;
}
} // namespace

juce::Result writeBpmOverride (const juce::File& packageDirectory,
                               const std::optional<SongMetadata::BpmOverride>& override)
{
    const auto file = packageDirectory.getChildFile ("song.json");
    const auto original = file.loadFileAsString();

    if (original.isEmpty())
        return juce::Result::fail ("Could not read " + file.getFullPathName() + ".");

    if (override.has_value()
        && ! (override->value >= static_cast<double> (dsp::tempoMinBpm)
              && override->value <= static_cast<double> (dsp::tempoMaxBpm)))
        return juce::Result::fail ("The tempo must be between " + juce::String (static_cast<int> (dsp::tempoMinBpm))
                                   + " and " + juce::String (static_cast<int> (dsp::tempoMaxBpm)) + " BPM.");

    int start = 0;
    int end = 0;

    if (! findOverrideValue (original, start, end))
        return juce::Result::fail ("song.json has no bpm.userOverride entry to update.");

    const auto replacement =
        override.has_value()
            ? "{ \"value\": " + juce::String (override->value, 4).trimCharactersAtEnd ("0").trimCharactersAtEnd (".")
                  + ", \"mode\": \"" + override->mode + "\" }"
            : juce::String ("null");
    const auto edited = original.substring (0, start) + replacement + original.substring (end);

    // The edit must still be a complete, valid song whose override is exactly what was asked for.
    const auto checked = parseSongMetadata (edited);

    if (! checked.isComplete())
        return juce::Result::fail ("The edited song.json would not be valid: " + checked.reason);

    const bool sameOverride = override.has_value() == checked.metadata.bpm.userOverride.has_value()
                           && (! override.has_value()
                               || (std::abs (checked.metadata.bpm.userOverride->value - override->value) < 1.0e-3
                                   && checked.metadata.bpm.userOverride->mode == override->mode));

    if (! sameOverride)
        return juce::Result::fail ("The edited song.json did not read back as the tempo that was set.");

    const juce::TemporaryFile temporary (file);

    const char* lineEnding = original.contains ("\r\n") ? "\r\n" : "\n";

    if (! temporary.getFile().replaceWithText (edited, false, false, lineEnding))
        return juce::Result::fail ("Could not write " + temporary.getFile().getFullPathName() + ".");

    if (! temporary.overwriteTargetFileWithTemporary())
        return juce::Result::fail ("Could not replace " + file.getFullPathName() + ".");

    return juce::Result::ok();
}
} // namespace snds
