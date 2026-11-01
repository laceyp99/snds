#include <juce_gui_basics/juce_gui_basics.h>

#include "DspHeavy.h"
#include "TestHelpers.h"
#include "rendering/ArtworkPalette.h"
#include "ui/FullscreenVisuals.h"
#include "ui/SongPanel.h"

#include <cmath>

namespace snds
{
namespace
{
using namespace rendering;

// The player's real colours: JUCE's default dark look and feel, and a panel brighter than the window.
const juce::Colour darkWindow (0xff323e44);
const juce::Colour darkPanel = darkWindow.brighter (0.15f);
const juce::Colour darkText = juce::Colours::white;
const juce::Colour lightPanel (0xfff2f2f2);
const juce::Colour lightText (0xff1a1a1a);

juce::Image solid (juce::Colour colour, int w = 64, int h = 64)
{
    juce::Image image (juce::Image::ARGB, w, h, true, juce::SoftwareImageType());
    juce::Graphics g (image);
    g.fillAll (colour);
    return image;
}

// A background with a patch of another colour covering `share` of the width.
juce::Image patch (juce::Colour background, juce::Colour patchColour, float share)
{
    juce::Image image (juce::Image::ARGB, 128, 128, true, juce::SoftwareImageType());
    juce::Graphics g (image);
    g.fillAll (background);
    g.setColour (patchColour);
    g.fillRect (0, 0, static_cast<int> (128.0f * share), 128);
    return image;
}

float hueDistance (float a, float b)
{
    const auto d = std::abs (a - b);
    return std::min (d, 1.0f - d);
}

class VisualPaletteTests final : public juce::UnitTest
{
public:
    VisualPaletteTests()
        : juce::UnitTest ("Visual palette", "snds")
    {
    }

    void runTest() override
    {
        testContrastMath();
        testEveryHueIsReadable();
        testDominance();
        testFallbacks();
        testRobustness();
        testSongPanel();
        testFullscreenBars();
    }

private:
    void testContrastMath()
    {
        beginTest ("The contrast ratio is the WCAG one");

        expectWithinAbsoluteError (contrastRatio (juce::Colours::black, juce::Colours::white), 21.0, 1.0e-9);
        expectWithinAbsoluteError (contrastRatio (juce::Colours::white, juce::Colours::black), 21.0, 1.0e-9);
        expectWithinAbsoluteError (contrastRatio (juce::Colours::red, juce::Colours::red), 1.0, 1.0e-9);
        expectWithinAbsoluteError (
            contrastRatio (juce::Colour (0xff767676), juce::Colours::white), 4.54, 0.01); // the classic AA grey
        expectWithinAbsoluteError (contrastRatio (juce::Colour (0xff595959), juce::Colours::white), 7.0, 0.02);
        expectWithinAbsoluteError (relativeLuminance (juce::Colours::white), 1.0, 1.0e-9);
        expectWithinAbsoluteError (relativeLuminance (juce::Colours::black), 0.0, 1.0e-9);

        // ensureContrast moves the colour the way that has room, keeps it when it is already fine.
        const juce::Colour darkBlue (0xff10204a);
        const auto onDark = ensureContrast (darkBlue, darkPanel, 4.5);
        expect (contrastRatio (onDark, darkPanel) >= 4.5 && onDark.getBrightness() > darkBlue.getBrightness());
        expect (hueDistance (onDark.getHue(), darkBlue.getHue()) < 0.03f); // still blue
        const auto onLight = ensureContrast (juce::Colour (0xffffe08a), lightPanel, 4.5);
        expect (contrastRatio (onLight, lightPanel) >= 4.5
                && onLight.getBrightness() < juce::Colour (0xffffe08a).getBrightness());
        expect (ensureContrast (juce::Colours::white, darkPanel, 4.5) == juce::Colours::white);

        // On a mid-grey panel neither white nor black always works; the better extreme is used.
        const juce::Colour mid (0xff7a7a7a);
        for (const auto start : { juce::Colours::red, juce::Colours::green, juce::Colour (0xff808080) })
            expect (contrastRatio (ensureContrast (start, mid, 4.5), mid) >= 4.5);
    }

    void testEveryHueIsReadable()
    {
        beginTest ("Whatever the artwork's colour, the accent, the text on it and the normal text stay readable");

        struct Look
        {
            const char* name;
            juce::Colour panel, text;
        };

        int checked = 0;
        for (const auto& look : { Look { "dark", darkPanel, darkText },
                                  Look { "light", lightPanel, lightText },
                                  Look { "near black", juce::Colour (0xff0c0c0c), juce::Colours::white } })
        {
            for (int hue = 0; hue < 360; hue += test::workload (40, 9))
            {
                for (const auto saturation :
                     test::workload<std::vector<float>> ({ 0.35f, 1.0f }, { 0.35f, 0.7f, 1.0f }))
                {
                    for (const auto brightness :
                         test::workload<std::vector<float>> ({ 0.25f, 0.9f }, { 0.25f, 0.5f, 0.8f, 1.0f }))
                    {
                        const auto art = solid (
                            juce::Colour::fromHSV (static_cast<float> (hue) / 360.0f, saturation, brightness, 1.0f));
                        const auto theme = makeThemePalette (art, look.panel, look.text);
                        const auto problems = validateThemePalette (theme, look.panel, look.text);
                        ++checked;

                        if (! problems.empty())
                            expect (false,
                                    juce::String (look.name) + " hue " + juce::String (hue) + " s "
                                        + juce::String (saturation) + " b " + juce::String (brightness) + ": "
                                        + problems.front());

                        if (theme.fromArtwork)
                        {
                            expect (contrastRatio (theme.accent, theme.panelBackground) >= 4.5);
                            expect (contrastRatio (theme.onAccent, theme.accent) >= 4.5);
                            expect (contrastRatio (look.text, theme.panelBackground) >= 7.0);
                        }
                    }
                }
            }
        }

        expect (checked > test::workload (
                    100, 1000)); // the default run covers every look and nine hues; SNDS_HEAVY_TESTS the full sweep

        // A bright, saturated colour keeps its hue (it needed no more than a small push on a dark panel).
        const auto blue = makeThemePalette (solid (juce::Colour (0xff3366ff)), darkPanel, darkText);
        expect (blue.fromArtwork && hueDistance (blue.accent.getHue(), juce::Colour (0xff3366ff).getHue()) < 0.02f);

        // The same input always gives the same palette.
        const auto a =
            makeThemePalette (patch (juce::Colour (0xff202020), juce::Colour (0xffe0a020), 0.4f), darkPanel, darkText);
        const auto b =
            makeThemePalette (patch (juce::Colour (0xff202020), juce::Colour (0xffe0a020), 0.4f), darkPanel, darkText);
        expect (a.accent == b.accent && a.onAccent == b.onAccent && a.panelBackground == b.panelBackground);

        // Real panels: the tint is a faint wash (the panel is still recognisably the panel).
        const auto tinted = makeThemePalette (solid (juce::Colour (0xffe03030)), darkPanel, darkText);
        expect (tinted.panelBackground != darkPanel);
        expect (std::abs (tinted.panelBackground.getBrightness() - darkPanel.getBrightness()) < 0.12f);
    }

    void testDominance()
    {
        beginTest ("The accent is the artwork's dominant vivid colour, not its darkest, greyest or rarest");

        const auto red = juce::Colour (0xffd02020);
        const auto blue = juce::Colour (0xff2040d0);
        const auto bigBlue = makeThemePalette (patch (blue, red, 0.15f), darkPanel, darkText);
        const auto bigRed = makeThemePalette (patch (blue, red, 0.85f), darkPanel, darkText);
        expect (bigBlue.fromArtwork && bigRed.fromArtwork);
        expect (hueDistance (bigBlue.accent.getHue(), blue.getHue()) < 0.04f);
        expect (hueDistance (bigRed.accent.getHue(), red.getHue()) < 0.04f);

        // Grey and black do not outvote a colour, however much of the picture they are.
        const auto mostlyGrey =
            makeThemePalette (patch (juce::Colour (0xff303030), juce::Colour (0xff20c060), 0.1f), darkPanel, darkText);
        expect (mostlyGrey.fromArtwork
                && hueDistance (mostlyGrey.accent.getHue(), juce::Colour (0xff20c060).getHue()) < 0.04f);

        // A few vivid pixels in grey artwork are too little to call the art coloured.
        const auto speck =
            makeThemePalette (patch (juce::Colour (0xff808080), juce::Colour (0xffff0000), 0.01f), darkPanel, darkText);
        expect (! speck.fromArtwork);
    }

    void testFallbacks()
    {
        beginTest ("Missing or unsuitable artwork gives the default styling, with the reason");

        struct Case
        {
            const char* name;
            juce::Image image;
            const char* reasonContains;
        };

        juce::Image see (juce::Image::ARGB, 64, 64, true); // fully transparent
        juce::Image faint (juce::Image::ARGB, 64, 64, true);
        {
            juce::Graphics g (faint);
            g.setColour (juce::Colour (0xffff0000).withAlpha (0.3f));
            g.fillAll();
        }

        for (const auto& c : { Case { "no image", {}, "no artwork" },
                               Case { "transparent", see, "transparent" },
                               Case { "mostly transparent", faint, "transparent" },
                               Case { "mid grey", solid (juce::Colour (0xff808080)), "no distinct colour" },
                               Case { "white", solid (juce::Colours::white), "no distinct colour" },
                               Case { "black", solid (juce::Colours::black), "no distinct colour" },
                               Case { "near black blue", solid (juce::Colour (0xff04060f)), "no distinct colour" },
                               Case { "pastel haze", solid (juce::Colour (0xfff4f0f2)), "no distinct colour" } })
        {
            const auto theme = makeThemePalette (c.image, darkPanel, darkText);
            expect (! theme.fromArtwork, c.name);
            expect (theme.reason.containsIgnoreCase (c.reasonContains), juce::String (c.name) + ": " + theme.reason);
            expect (theme.panelBackground == darkPanel, c.name); // no tint at all
            expect (validateThemePalette (theme, darkPanel, darkText).empty(), c.name);
            expect (contrastRatio (theme.accent, darkPanel) >= 4.5
                        && contrastRatio (theme.onAccent, theme.accent) >= 4.5,
                    c.name);

            const auto def = defaultThemePalette (darkPanel, darkText);
            expect (theme.accent == def.accent && theme.onAccent == def.onAccent,
                    c.name); // the default styling, always the same
        }

        expect (defaultThemePalette (lightPanel, lightText).accent != defaultThemePalette (darkPanel, darkText).accent
                || contrastRatio (defaultThemePalette (lightPanel, lightText).accent, lightPanel) >= 4.5);
        expect (contrastRatio (defaultThemePalette (lightPanel, lightText).accent, lightPanel) >= 4.5);
    }

    void testRobustness()
    {
        beginTest ("Odd images never break the palette");

        for (const auto& size : { std::pair { 1, 1 },
                                  std::pair { 2, 3 },
                                  std::pair { 4000, 3 },
                                  std::pair { 3, 4000 },
                                  std::pair { 31, 33 } })
        {
            const auto theme =
                makeThemePalette (solid (juce::Colour (0xff20a0e0), size.first, size.second), darkPanel, darkText);
            expect (validateThemePalette (theme, darkPanel, darkText).empty(),
                    juce::String (size.first) + "x" + juce::String (size.second));
            expect (theme.fromArtwork);
        }

        // A photo-like gradient of many colours: a deterministic, valid answer.
        juce::Image gradient (juce::Image::ARGB, 200, 200, true);
        {
            juce::Graphics g (gradient);
            g.setGradientFill (juce::ColourGradient (
                juce::Colour (0xffff5f6d), 0.0f, 0.0f, juce::Colour (0xff1d2b64), 200.0f, 200.0f, false));
            g.fillAll();
        }
        const auto theme = makeThemePalette (gradient, darkPanel, darkText);
        expect (theme.fromArtwork && validateThemePalette (theme, darkPanel, darkText).empty());

        // A problem the validator can see.
        auto bad = theme;
        bad.accent = darkPanel; // invisible on its own panel
        expect (! validateThemePalette (bad, darkPanel, darkText).empty());
    }

    //==========================================================================
    static OpenSong songWithArtwork (const juce::File& directory, const juce::Image* image)
    {
        OpenSong song;
        song.entry.song = SongMetadata {};
        song.entry.song->source.fileName = "song.wav";
        song.entry.directory = directory;

        if (image != nullptr)
        {
            juce::PNGImageFormat png;
            juce::FileOutputStream out (directory.getChildFile ("art.png"));
            out.setPosition (0);
            out.truncate();
            png.writeImageToStream (*image, out);
            song.entry.song->artwork.file = "art.png";
        }

        return song;
    }

    void testSongPanel()
    {
        beginTest ("The player's accents follow the song's artwork, and fall back to the default styling");

        const test::ScopedTempDirectory temp;
        SongPanel panel;
        panel.setSize (900, 340);
        const auto panelColour = panel.findColour (juce::ResizableWindow::backgroundColourId).brighter (0.15f);
        const auto text = panel.findColour (juce::Label::textColourId);

        expect (! panel.getTheme().fromArtwork); // nothing open: the default styling
        expect (! panel.intensityControl().isColourSpecified (juce::Slider::thumbColourId));

        // Red artwork: the accents turn red, and every pairing is readable.
        const auto red = solid (juce::Colour (0xffd83030));
        panel.setSong (songWithArtwork (temp.get(), &red));
        auto theme = panel.getTheme();
        expect (theme.fromArtwork);
        expect (hueDistance (theme.accent.getHue(), juce::Colour (0xffd83030).getHue()) < 0.04f);
        expect (validateThemePalette (theme, panelColour, text).empty());
        expect (panel.intensityControl().findColour (juce::Slider::thumbColourId) == theme.accent);
        expect (panel.intensityControl().findColour (juce::Slider::trackColourId) == theme.accent);
        expect (panel.effectsControl().findColour (juce::ToggleButton::tickColourId) == theme.accent);
        expect (panel.fullscreenControl().findColour (juce::TextButton::buttonColourId) == theme.accent);
        expect (panel.fullscreenControl().findColour (juce::TextButton::textColourOffId) == theme.onAccent);
        expect (contrastRatio (panel.fullscreenControl().findColour (juce::TextButton::textColourOffId),
                               panel.fullscreenControl().findColour (juce::TextButton::buttonColourId))
                >= 4.5);

        // Different artwork, different accent.
        const auto green = solid (juce::Colour (0xff30c050));
        panel.setSong (songWithArtwork (temp.get(), &green));
        expect (panel.getTheme().fromArtwork
                && hueDistance (panel.getTheme().accent.getHue(), juce::Colour (0xff30c050).getHue()) < 0.04f);

        // Grey artwork is unsuitable: the default styling comes back (the colours are removed, not set to something).
        const auto grey = solid (juce::Colour (0xff909090));
        panel.setSong (songWithArtwork (temp.get(), &grey));
        expect (! panel.getTheme().fromArtwork && panel.getTheme().reason.isNotEmpty());
        expect (! panel.intensityControl().isColourSpecified (juce::Slider::thumbColourId));
        expect (! panel.effectsControl().isColourSpecified (juce::ToggleButton::tickColourId));
        expect (! panel.fullscreenControl().isColourSpecified (juce::TextButton::buttonColourId));

        // No artwork file at all: the song gets the generated artwork, which is coloured, so it themes the panel.
        panel.setSong (songWithArtwork (temp.get().getChildFile ("nothing-here"), nullptr));
        expect (panel.getTheme().fromArtwork && validateThemePalette (panel.getTheme(), panelColour, text).empty());

        // An artwork file that is not an image falls back to the generated artwork too.
        auto broken = songWithArtwork (temp.get(), nullptr);
        temp.child ("broken.png").replaceWithText ("this is not a png");
        broken.entry.song->artwork.file = "broken.png";
        panel.setSong (broken);
        expect (panel.getTheme().fromArtwork);

        // The song closes: the default styling, and the controls go back to the look and feel's colours.
        panel.setSong (std::nullopt);
        expect (! panel.getTheme().fromArtwork);
        expect (! panel.intensityControl().isColourSpecified (juce::Slider::thumbColourId));

        // The panel's text keeps its contrast on the tinted panel, for any artwork.
        for (const auto hue : { 0.0f, 0.15f, 0.3f, 0.5f, 0.65f, 0.8f })
        {
            const auto art = solid (juce::Colour::fromHSV (hue, 0.9f, 0.9f, 1.0f));
            panel.setSong (songWithArtwork (temp.get(), &art));
            expect (contrastRatio (text, panel.getTheme().panelBackground) >= 7.0, "hue " + juce::String (hue));
        }
    }

    void testFullscreenBars()
    {
        beginTest (
            "The fullscreen bars take a faint wash of the accent, keep their text readable, and the exit stays red");

        for (int hue = 0; hue < 360; hue += test::workload (60, 15))
        {
            const auto theme =
                makeThemePalette (solid (juce::Colour::fromHSV (static_cast<float> (hue) / 360.0f, 0.9f, 0.95f, 1.0f)),
                                  darkPanel,
                                  darkText);
            const auto bar = FullscreenVisualsContent::barColourFor (theme);
            expect (contrastRatio (juce::Colours::white, bar) >= 7.0, "hue " + juce::String (hue));
            expect (std::abs (bar.getBrightness() - FullscreenVisualsContent::defaultBarColour().getBrightness())
                    < 0.12f);
        }

        expect (FullscreenVisualsContent::barColourFor (defaultThemePalette (darkPanel, darkText))
                == FullscreenVisualsContent::defaultBarColour());

        VisualHost host (RenderOverrides::parse ("no-opengl"));
        host.setSize (100, 100);
        VisualController visuals (host);
        FullscreenActions actions;
        actions.view = [] { return PlaybackView(); };
        FullscreenVisualsContent content (host, visuals, actions, "Song");

        const auto exitColour = content.exitControl().findColour (juce::TextButton::buttonColourId);
        const auto theme = makeThemePalette (solid (juce::Colour (0xff20a0e0)), darkPanel, darkText);
        content.setTheme (theme);

        expect (content.getBarColour() == FullscreenVisualsContent::barColourFor (theme));
        const auto thumb = content.positionControl().findColour (juce::Slider::thumbColourId);
        expect (contrastRatio (thumb, content.getBarColour()) >= 4.5);
        expect (contrastRatio (content.playControl().findColour (juce::TextButton::textColourOffId),
                               content.playControl().findColour (juce::TextButton::buttonColourId))
                >= 4.5);
        expect (content.exitControl().findColour (juce::TextButton::buttonColourId) == exitColour); // never themed
        expect (contrastRatio (juce::Colours::white, exitColour) >= 4.5);

        content.setTheme (defaultThemePalette (darkPanel, darkText));
        expect (content.getBarColour() == FullscreenVisualsContent::defaultBarColour());
        expect (! content.positionControl().isColourSpecified (juce::Slider::thumbColourId));
        expect (! content.playControl().isColourSpecified (juce::TextButton::buttonColourId));

        content.releaseHost();
    }
};

VisualPaletteTests visualPaletteTests;
} // namespace
} // namespace snds
