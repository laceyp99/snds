#include "ImportFixtures.h"
#include "app/EmbeddedArtwork.h"
#include "app/SongImport.h"
#include "rendering/FallbackArtwork.h"

namespace snds::test
{

class EmbeddedArtworkTests final : public juce::UnitTest
{
public:
    EmbeddedArtworkTests()
        : juce::UnitTest ("Embedded artwork", testCategory)
    {
    }

    void runTest() override
    {
        const auto front = makePng (40, 20, juce::Colours::teal);
        const auto other = makePng (8, 8, juce::Colours::orange);
        const auto payload = unsyncTorturePayload();

        beginTest ("ID3v2.3 APIC frames are parsed in order with their types");
        {
            const auto tag = makeId3Tag ({
                { "TIT2", juce::MemoryBlock ("\0Title", 6) },
                { "APIC", apicBody ("image/png", 0, "other", other) },
                { "APIC", apicBody ("image/png", 3, "front", front) },
            });
            const auto pictures = parseId3Pictures (tag.getData(), tag.getSize());
            expectEquals (static_cast<int> (pictures.size()), 2);

            if (pictures.size() == 2)
            {
                expectEquals (pictures[0].pictureType, 0);
                expectEquals (pictures[1].pictureType, id3FrontCoverType);
                expectEquals (pictures[1].mimeType, juce::String ("image/png"));
                expectEquals (pictures[1].description, juce::String ("front"));
                expect (pictures[1].data == front);
            }
        }

        beginTest ("ID3v2.3 tag-level unsynchronisation and extended header");
        {
            Id3TagOptions options;
            options.unsynchronise = true;
            options.extendedHeader = true;
            const auto tag = makeId3Tag ({ { "APIC", apicBody ("image/png", 3, "", payload) } }, options);
            expect (tag.getSize() > payload.getSize() + 20, "the tag really was unsynchronised");

            const auto pictures = parseId3Pictures (tag.getData(), tag.getSize());
            expect (pictures.size() == 1 && pictures[0].data == payload);
        }

        beginTest ("ID3v2.4 frame unsynchronisation, grouping, and data length indicator");
        {
            const auto body = apicBody ("image/png", 3, "", payload);
            ByteWriter stored;
            stored.u8 (7); // group identifier
            writeSyncsafe (stored, static_cast<juce::uint32> (body.getSize()));
            stored.bytes (unsynchronise (body));

            Id3TagOptions options;
            options.majorVersion = 4;
            options.extendedHeader = true;
            const auto tag = makeId3Tag ({ { "APIC", stored.getBlock(), 0x0043 } }, options);

            const auto pictures = parseId3Pictures (tag.getData(), tag.getSize());
            expect (pictures.size() == 1 && pictures[0].data == payload);
        }

        beginTest ("ID3v2.4 tags written with v2.3-style frame sizes are tolerated");
        {
            Id3TagOptions options;
            options.majorVersion = 4;
            options.plainSizesInV24 = true;
            const auto tag = makeId3Tag (
                { { "TIT2", juce::MemoryBlock ("\0Title", 6) }, { "APIC", apicBody ("image/png", 3, "", front) } },
                options);
            const auto pictures = parseId3Pictures (tag.getData(), tag.getSize());
            expect (pictures.size() == 1 && pictures[0].data == front);
        }

        beginTest ("zlib-compressed APIC frames are inflated");
        {
            const auto body = apicBody ("image/png", 3, "", front);
            juce::MemoryOutputStream compressed;

            {
                juce::GZIPCompressorOutputStream zlib (compressed, 9);
                zlib.write (body.getData(), body.getSize());
            }

            ByteWriter stored;
            writeBigEndian (stored, static_cast<juce::uint32> (body.getSize()), 4);
            stored.bytes (compressed.getMemoryBlock());

            const auto tag = makeId3Tag ({ { "APIC", stored.getBlock(), 0x0080 } });
            const auto pictures = parseId3Pictures (tag.getData(), tag.getSize());
            expect (pictures.size() == 1 && pictures[0].data == front);
        }

        beginTest ("ID3v2.2 PIC frames and UTF-16 descriptions");
        {
            ByteWriter pic;
            pic.u8 (1)
                .text ("PNG")
                .u8 (3)
                .bytes ("\xff\xfe"
                        "F\0r\0o\0n\0t\0\0\0",
                        14)
                .bytes (front);

            Id3TagOptions options;
            options.majorVersion = 2;
            const auto tag = makeId3Tag ({ { "PIC", pic.getBlock() } }, options);
            const auto pictures = parseId3Pictures (tag.getData(), tag.getSize());
            expect (pictures.size() == 1);

            if (pictures.size() == 1)
            {
                expectEquals (pictures[0].mimeType, juce::String ("image/png"));
                expectEquals (pictures[0].description, juce::String ("Front"));
                expect (pictures[0].data == front);
            }
        }

        beginTest ("Encrypted frames and linked images are skipped");
        {
            const auto tag = makeId3Tag ({
                { "APIC", apicBody ("image/png", 3, "", front), 0x0040 }, // encrypted (v2.3)
                { "APIC", apicBody ("-->", 3, "", juce::MemoryBlock ("http://example", 14)) },
                { "APIC", apicBody ("image/png", 0, "", other) },
            });
            const auto pictures = parseId3Pictures (tag.getData(), tag.getSize());
            expect (pictures.size() == 1 && pictures[0].data == other);
        }

        beginTest ("Truncated and mutated tags never overrun");
        {
            const auto tag = makeId3Tag (
                { { "APIC", apicBody ("image/png", 0, "", other) }, { "APIC", apicBody ("image/png", 3, "", front) } });

            for (size_t length = 0; length <= tag.getSize(); length += 7)
                parseId3Pictures (tag.getData(), length);

            const auto firstOnly = parseId3Pictures (tag.getData(), tag.getSize() - front.getSize());
            expect (firstOnly.size() == 1 && firstOnly[0].data == other, "pictures before the damage survive");

            juce::Random random (42);

            for (auto iteration = 0; iteration < 300; ++iteration)
            {
                auto mutated = tag;
                auto* bytes = static_cast<juce::uint8*> (mutated.getData());

                for (auto flips = 0; flips < 4; ++flips)
                    bytes[10 + random.nextInt (static_cast<int> (mutated.getSize()) - 10)] =
                        static_cast<juce::uint8> (random.nextInt (256));

                parseId3Pictures (mutated.getData(), mutated.getSize());
            }

            expect (parseId3Pictures (nullptr, 0).empty());
            expect (parseId3Pictures ("ID3", 3).empty());
        }

        beginTest ("MP3 fixtures: front cover preferred, v2.4 unsynchronised art, none without a tag");
        {
            const auto withArt = extractFromFixture ("tone-stereo-44k1-apic.mp3");
            expect (withArt.artwork.has_value(), withArt.note);

            if (withArt.artwork.has_value())
            {
                const auto& image = withArt.artwork->image;
                expectEquals (image.getWidth(), 48);
                expectEquals (image.getHeight(), 32);
                expect (withArt.artwork->origin == model::ArtworkOrigin::embedded);
                expect (withArt.artwork->description.contains ("front cover"), withArt.artwork->description);
                const auto pixel = image.getPixelAt (0, 0);
                expect (pixel.getRed() == 40 && pixel.getGreen() == 90 && pixel.getBlue() == 200,
                        pixel.toDisplayString (false));
            }

            const auto unsync = extractFromFixture ("tone-mono-48k-v24-unsync.mp3");
            expect (unsync.artwork.has_value() && unsync.artwork->image.getWidth() == 48, unsync.note);

            const auto none = extractFromFixture ("tone-stereo-44k1.mp3");
            expect (! none.artwork.has_value());
            expect (none.note.contains ("no ID3v2 tag"), none.note);
        }

        beginTest ("WAV id3 chunks carry artwork");
        {
            const ScopedTempDirectory temp;
            const auto tag =
                makeId3Tag ({ { "APIC", apicBody ("image/png", 3, "", makePng (20, 10, juce::Colours::red)) } });

            for (const auto upperCase : { false, true })
            {
                WavSpec spec;
                spec.id3Payload = tag;
                spec.id3ChunkUpperCase = upperCase;
                spec.id3BeforeData = upperCase;
                const auto file = writeFile (temp, upperCase ? "upper.wav" : "lower.wav", makeWav (spec));

                const auto source = inspectSource (file);
                expect (source.wasOk(), source.error.message);

                if (! source.wasOk())
                    continue;

                const auto extracted = extractEmbeddedArtwork (*source.source);
                expect (extracted.artwork.has_value(), extracted.note);

                if (extracted.artwork.has_value())
                {
                    expectEquals (extracted.artwork->image.getWidth(), 20);
                    expectEquals (extracted.artwork->image.getHeight(), 10);
                }
            }

            const auto plain = inspectSource (writeFile (temp, "plain.wav", makeWav ({})));
            expect (plain.wasOk());

            if (plain.wasOk())
            {
                const auto extracted = extractEmbeddedArtwork (*plain.source);
                expect (! extracted.artwork.has_value());
                expect (extracted.note.contains ("WAV"), extracted.note);
            }
        }

        beginTest ("An undecodable front cover falls through to other pictures");
        {
            const ScopedTempDirectory temp;
            const auto garbage = juce::MemoryBlock ("\x89PNG\r\n\x1a\nnot really a png", 24);

            WavSpec spec;
            spec.id3Payload = makeId3Tag ({ { "APIC", apicBody ("image/png", 3, "", garbage) },
                                            { "APIC", apicBody ("image/png", 0, "", other) } });
            auto source = inspectSource (writeFile (temp, "fallthrough.wav", makeWav (spec)));
            expect (source.wasOk());

            if (source.wasOk())
            {
                const auto extracted = extractEmbeddedArtwork (*source.source);
                expect (extracted.artwork.has_value() && extracted.artwork->image.getWidth() == 8);
                expect (extracted.note.contains ("could not be decoded"), extracted.note);
            }

            spec.id3Payload = makeId3Tag ({ { "APIC", apicBody ("image/png", 3, "", garbage) } });
            source = inspectSource (writeFile (temp, "broken.wav", makeWav (spec)));

            if (source.wasOk())
            {
                const auto extracted = extractEmbeddedArtwork (*source.source);
                expect (! extracted.artwork.has_value());
                expect (extracted.note.contains ("could not be decoded"), extracted.note);
            }
        }

        beginTest ("Image decoding is guarded and downscales huge images");
        {
            const auto png = decodeArtworkImage (front.getData(), front.getSize());
            expect (png.image.isValid() && png.imageType == "PNG" && png.error.isEmpty());
            expect (png.image.getFormat() == juce::Image::ARGB);

            const auto jpegBytes = makeJpeg (30, 20, juce::Colours::navy);
            const auto jpeg = decodeArtworkImage (jpegBytes.getData(), jpegBytes.getSize());
            expect (jpeg.image.isValid() && jpeg.imageType == "JPEG" && jpeg.image.getWidth() == 30);

            expect (decodeArtworkImage (nullptr, 0).error.isNotEmpty());
            expect (decodeArtworkImage ("hello world", 11).error.contains ("not a PNG"));

            auto bomb = makePng (4, 4, juce::Colours::white);
            auto* ihdr = static_cast<juce::uint8*> (bomb.getData()) + 16;
            const juce::uint8 huge[8] = { 0, 0, 0x4e, 0x20, 0, 0, 0x4e, 0x20 }; // 20000 x 20000
            std::memcpy (ihdr, huge, sizeof (huge));
            expect (decodeArtworkImage (bomb.getData(), bomb.getSize()).error.contains ("too large"));

            const auto wide = makePng (3000, 1000, juce::Colours::green);
            const auto scaled = decodeArtworkImage (wide.getData(), wide.getSize());
            expectEquals (scaled.sourceWidth, 3000);
            expectEquals (scaled.image.getWidth(), ArtworkLimits::maxStoredDimension);
            expectEquals (scaled.image.getHeight(), 683);
        }

        beginTest ("Replacement artwork loads PNG and JPEG and explains failures");
        {
            const ScopedTempDirectory temp;
            const auto pngFile = writeFile (temp, "cover.png", front);
            const auto jpgFile = writeFile (temp, "cover.jpeg", makeJpeg (16, 16, juce::Colours::pink));

            for (const auto& file : { pngFile, jpgFile })
            {
                const auto loaded = loadReplacementArtwork (file);
                expect (loaded.artwork.has_value(), loaded.error);

                if (loaded.artwork.has_value())
                {
                    expect (loaded.artwork->origin == model::ArtworkOrigin::replacement);
                    expect (loaded.artwork->file == file);
                    expect (loaded.artwork->description.contains (file.getFileName()));
                }
            }

            expect (
                loadReplacementArtwork (temp.get().getChildFile ("missing.png")).error.contains ("no longer exists"));
            expect (loadReplacementArtwork (writeFile (temp, "cover.bmp", front)).error.contains ("PNG or JPEG"));
            expect (loadReplacementArtwork (writeFile (temp, "fake.jpg", juce::MemoryBlock ("nope", 4)))
                        .error.contains ("could not use"));
        }

        beginTest ("Fallback artwork is deterministic per file name");
        {
            using namespace rendering;
            expect (fallbackArtworkSeed ("Song Title") == fallbackArtworkSeed ("song title"));
            expect (fallbackArtworkSeed ("Song A") != fallbackArtworkSeed ("Song B"));

            const auto a1 = generateFallbackArtwork (fallbackArtworkSeed ("Song A"), 128);
            const auto a2 = generateFallbackArtwork (fallbackArtworkSeed ("Song A"), 128);
            const auto b = generateFallbackArtwork (fallbackArtworkSeed ("Song B"), 128);

            expectEquals (a1.getWidth(), 128);
            expectEquals (a1.getHeight(), 128);
            expect (a1.getFormat() == juce::Image::ARGB);
            expect (samePixels (a1, a2), "same seed, same pixels");
            expect (! samePixels (a1, b), "different seed, different pixels");
            expect (a1.getPixelAt (10, 10) != a1.getPixelAt (64, 64)
                        || a1.getPixelAt (64, 64) != a1.getPixelAt (120, 30),
                    "not a flat colour");
        }

        beginTest ("Artwork choice: embedded first, selection changes withdraw confirmation");
        {
            model::Artwork embedded;
            embedded.image = juce::Image (juce::Image::ARGB, 4, 4, true, juce::SoftwareImageType());
            embedded.origin = model::ArtworkOrigin::embedded;
            model::Artwork fallback;
            fallback.image = juce::Image (juce::Image::ARGB, 8, 8, true, juce::SoftwareImageType());
            model::Artwork replacement;
            replacement.image = juce::Image (juce::Image::ARGB, 2, 2, true, juce::SoftwareImageType());

            model::ArtworkChoice choice (embedded, fallback);
            expect (choice.getSelectedOrigin() == model::ArtworkOrigin::embedded);
            expect (! choice.isConfirmed());
            choice.confirm();
            expect (choice.isConfirmed());

            choice.selectFallback();
            expect (! choice.isConfirmed());
            expectEquals (choice.getSelected().image.getWidth(), 8);

            choice.selectReplacement (replacement);
            expect (choice.getSelectedOrigin() == model::ArtworkOrigin::replacement);
            expect (choice.getSelected().origin == model::ArtworkOrigin::replacement);
            expectEquals (choice.getSelected().image.getWidth(), 2);

            choice.selectEmbedded();
            expectEquals (choice.getSelected().image.getWidth(), 4);

            model::ArtworkChoice withoutEmbedded (std::nullopt, fallback);
            expect (withoutEmbedded.getSelectedOrigin() == model::ArtworkOrigin::fallback);
            withoutEmbedded.selectEmbedded();
            expect (withoutEmbedded.getSelectedOrigin() == model::ArtworkOrigin::fallback);
        }
    }

private:
    static EmbeddedArtworkResult extractFromFixture (const char* name)
    {
        const auto result = inspectSource (getImportFixture (name));

        if (! result.wasOk())
            return { std::nullopt, "fixture rejected: " + result.error.message };

        return extractEmbeddedArtwork (*result.source);
    }

    static bool samePixels (const juce::Image& a, const juce::Image& b)
    {
        if (a.getWidth() != b.getWidth() || a.getHeight() != b.getHeight())
            return false;

        for (auto y = 0; y < a.getHeight(); ++y)
            for (auto x = 0; x < a.getWidth(); ++x)
                if (a.getPixelAt (x, y) != b.getPixelAt (x, y))
                    return false;

        return true;
    }
};

static EmbeddedArtworkTests embeddedArtworkTests;

} // namespace snds::test
