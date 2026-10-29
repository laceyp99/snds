#pragma once

namespace snds::model
{

/** Top-level screens of the application shell. The main component shows exactly one at a time. */
enum class AppScreen
{
    readiness,  ///< Environment readiness and setup guidance.
    songImport, ///< Choose a WAV/MP3 source and confirm artwork.
    player,     ///< Transport, stem mix, and reactive artwork.
};

constexpr const char* toString (AppScreen screen) noexcept
{
    switch (screen)
    {
        case AppScreen::readiness:
            return "readiness";
        case AppScreen::songImport:
            return "import";
        case AppScreen::player:
            return "player";
    }

    return "unknown";
}

/** Steps of the song import screen. */
enum class ImportStage
{
    chooseSource,   ///< Waiting for the user to pick a WAV/MP3 file.
    inspecting,     ///< Validating the file and reading its artwork (background thread).
    problem,        ///< The file was rejected; shows the reason and what to do.
    confirmArtwork, ///< Confirm, replace, or fall back from the detected artwork.
    prepare,        ///< Confirmed source and artwork; separation starts here once available.
};

constexpr const char* toString (ImportStage stage) noexcept
{
    switch (stage)
    {
        case ImportStage::chooseSource:
            return "choose-source";
        case ImportStage::inspecting:
            return "inspecting";
        case ImportStage::problem:
            return "problem";
        case ImportStage::confirmArtwork:
            return "confirm-artwork";
        case ImportStage::prepare:
            return "prepare";
    }

    return "unknown";
}

} // namespace snds::model
