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

} // namespace snds::model
