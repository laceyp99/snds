#include <juce_graphics/juce_graphics.h>

#include "TestHelpers.h"
#include "model/Artwork.h"

#include <cstring>
#include <vector>

namespace snds
{
namespace
{
using Bytes = std::vector<unsigned char>;

void append (Bytes& out, const void* data, size_t size)
{
    const auto* p = static_cast<const unsigned char*> (data);
    out.insert (out.end(), p, p + size);
}

void append (Bytes& out, const char* text)
{
    append (out, text, std::strlen (text));
}

void appendBe32 (Bytes& out, juce::uint32 v)
{
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back (static_cast<unsigned char> ((v >> shift) & 0xFF));
}

void appendSyncsafe (Bytes& out, juce::uint32 v)
{
    for (int shift = 21; shift >= 0; shift -= 7)
        out.push_back (static_cast<unsigned char> ((v >> shift) & 0x7F));
}

Bytes unsynchronise (const Bytes& in)
{
    Bytes out;

    for (size_t i = 0; i < in.size(); ++i)
    {
        out.push_back (in[i]);

        if (in[i] == 0xFF && (i + 1 == in.size() || in[i + 1] == 0 || in[i + 1] >= 0xE0))
            out.push_back (0);
    }

    return out;
}

//==============================================================================
// Synthetic pictures: drawn here, never copied from anywhere.
Bytes pngBytes (int size = 16)
{
    juce::Image image (juce::Image::ARGB, size, size, true);
    image.clear (image.getBounds(), juce::Colour (0xff3366cc));
    const auto block = encodePng (image);
    return Bytes (static_cast<const unsigned char*> (block.getData()),
                  static_cast<const unsigned char*> (block.getData()) + block.getSize());
}

Bytes jpegBytes()
{
    juce::Image image (juce::Image::RGB, 16, 16, true);
    image.clear (image.getBounds(), juce::Colour (0xffcc6633));
    juce::MemoryOutputStream out;
    juce::JPEGImageFormat jpeg;
    jpeg.writeImageToStream (image, out);
    const auto block = out.getMemoryBlock();
    return Bytes (static_cast<const unsigned char*> (block.getData()),
                  static_cast<const unsigned char*> (block.getData()) + block.getSize());
}

//==============================================================================
struct Frame
{
    Bytes body;
    int flags = 0; // the two ID3 frame flag bytes
    juce::String id = "APIC";
};

Frame apic (const Bytes& image,
            int pictureType = 3,
            const char* mime = "image/png",
            int encoding = 0,
            const Bytes& description = { 'c', 'o', 'v', 'e', 'r', 0 })
{
    Frame frame;
    frame.body.push_back (static_cast<unsigned char> (encoding));
    append (frame.body, mime);
    frame.body.push_back (0);
    frame.body.push_back (static_cast<unsigned char> (pictureType));
    append (frame.body, description.data(), description.size());
    append (frame.body, image.data(), image.size());
    return frame;
}

Frame pic22 (const Bytes& image, const char* format = "PNG", int pictureType = 3)
{
    Frame frame;
    frame.id = "PIC";
    frame.body.push_back (0);
    append (frame.body, format);
    frame.body.push_back (static_cast<unsigned char> (pictureType));
    frame.body.push_back (0); // empty description
    append (frame.body, image.data(), image.size());
    return frame;
}

Frame textFrame (const char* id, const char* text)
{
    Frame frame;
    frame.id = id;
    frame.body.push_back (0);
    append (frame.body, text);
    return frame;
}

struct TagOptions
{
    int major = 3;
    int headerFlags = 0;
    bool extendedHeader = false;
    bool syncsafeFrameSizes = true; // v2.4 only
    int paddingBytes = 0;
    bool unsynchroniseWholeTag = false; // v2.3 flag 0x80
};

Bytes buildTag (const std::vector<Frame>& frames, const TagOptions& options)
{
    Bytes body;

    if (options.extendedHeader)
    {
        if (options.major == 4)
        {
            appendSyncsafe (body, 6);
            body.push_back (1);
            body.push_back (0);
        }
        else
        {
            appendBe32 (body, 6);
            body.push_back (0);
            body.push_back (0);
            appendBe32 (body, 0); // padding size field of the v2.3 extended header
        }
    }

    for (const auto& frame : frames)
    {
        if (options.major == 2)
        {
            append (body, frame.id.toRawUTF8(), 3);
            body.push_back (static_cast<unsigned char> ((frame.body.size() >> 16) & 0xFF));
            body.push_back (static_cast<unsigned char> ((frame.body.size() >> 8) & 0xFF));
            body.push_back (static_cast<unsigned char> (frame.body.size() & 0xFF));
        }
        else
        {
            append (body, frame.id.toRawUTF8(), 4);

            if (options.major == 4 && options.syncsafeFrameSizes)
                appendSyncsafe (body, static_cast<juce::uint32> (frame.body.size()));
            else
                appendBe32 (body, static_cast<juce::uint32> (frame.body.size()));

            body.push_back (static_cast<unsigned char> (frame.flags >> 8));
            body.push_back (static_cast<unsigned char> (frame.flags & 0xFF));
        }

        append (body, frame.body.data(), frame.body.size());
    }

    body.resize (body.size() + static_cast<size_t> (options.paddingBytes), 0);

    if (options.unsynchroniseWholeTag)
        body = unsynchronise (body);

    Bytes tag;
    append (tag, "ID3");
    tag.push_back (static_cast<unsigned char> (options.major));
    tag.push_back (0);
    tag.push_back (static_cast<unsigned char> (options.headerFlags | (options.extendedHeader ? 0x40 : 0)
                                               | (options.unsynchroniseWholeTag ? 0x80 : 0)));
    appendSyncsafe (tag, static_cast<juce::uint32> (body.size()));
    append (tag, body.data(), body.size());
    return tag;
}

//==============================================================================
class ArtworkTests final : public juce::UnitTest
{
public:
    ArtworkTests()
        : juce::UnitTest ("Artwork", "snds")
    {
    }

    void runTest() override
    {
        beginTest ("Setup");
        runId3Tests();
        runRobustnessTests();
        runFileTests();
        runFallbackAndChoiceTests();
    }

private:
    void expectPicture (const std::optional<ImageData>& found,
                        const Bytes& expected,
                        const juce::String& extension,
                        const juce::String& what)
    {
        expect (found.has_value(), what + ": a picture is found");

        if (! found)
            return;

        expectEquals (found->extension, extension, what);
        expect (found->bytes.getSize() == expected.size()
                    && std::memcmp (found->bytes.getData(), expected.data(), expected.size()) == 0,
                what + ": the bytes are intact");
    }

    std::optional<ImageData> parse (const Bytes& tag) { return findArtworkInId3 (tag.data(), tag.size()); }

    void runId3Tests()
    {
        const auto png = pngBytes();
        const auto jpg = jpegBytes();

        beginTest ("ID3v2.3 APIC with a PNG, between text frames");
        {
            const auto tag = buildTag ({ textFrame ("TIT2", "Song"), apic (png), textFrame ("TPE1", "Artist") }, {});
            expectPicture (parse (tag), png, "png", "v2.3");
            expectEquals (parse (tag)->pictureType, 3);
        }

        beginTest ("ID3v2.4 with syncsafe sizes, a UTF-16 description, and a data length indicator");
        {
            auto frame = apic (jpg, 3, "image/jpeg", 1, { 0xFF, 0xFE, 'a', 0, 'b', 0, 0, 0 });
            frame.flags = 0x0001; // data length indicator
            Bytes withIndicator;
            appendSyncsafe (withIndicator, static_cast<juce::uint32> (frame.body.size()));
            append (withIndicator, frame.body.data(), frame.body.size());
            frame.body = withIndicator;
            TagOptions options;
            options.major = 4;
            expectPicture (parse (buildTag ({ frame }, options)), jpg, "jpg", "v2.4");
        }

        beginTest ("ID3v2.4 frame sizes written as plain integers (a common tagger bug) still parse");
        {
            const Bytes big = pngBytes (64); // a body well over 127 bytes
            TagOptions options;
            options.major = 4;
            options.syncsafeFrameSizes = false;
            expectPicture (parse (buildTag ({ apic (big) }, options)), big, "png", "v2.4 non-syncsafe");
        }

        beginTest ("ID3v2.2 PIC frames");
        {
            TagOptions options;
            options.major = 2;
            expectPicture (parse (buildTag ({ pic22 (png) }, options)), png, "png", "v2.2");
            expectPicture (parse (buildTag ({ pic22 (jpg, "JPG") }, options)), jpg, "jpg", "v2.2 jpg");
        }

        beginTest ("The front cover wins over an earlier picture of another type");
        {
            const auto other = pngBytes (8);
            const auto tag = buildTag ({ apic (other, 0), apic (jpg, 3, "image/jpeg") }, {});
            expectPicture (parse (tag), jpg, "jpg", "front cover");
            expectPicture (
                parse (buildTag ({ apic (other, 0), apic (png, 4) }, {})), other, "png", "first when no cover");
        }

        beginTest ("The unsynchronisation flag is undone before parsing");
        {
            const auto jpeg = jpegBytes();
            expect (jpeg.size() > 4);
            TagOptions options;
            options.unsynchroniseWholeTag = true;
            expectPicture (
                parse (buildTag ({ apic (jpeg, 3, "image/jpeg") }, options)), jpeg, "jpg", "unsynchronised v2.3");

            TagOptions v4;
            v4.major = 4;
            auto frame = apic (jpeg, 3, "image/jpeg");
            frame.body = unsynchronise (frame.body);
            frame.flags = 0x0002;
            expectPicture (parse (buildTag ({ frame }, v4)), jpeg, "jpg", "frame-level unsynchronisation");
        }

        beginTest ("Extended headers and padding are skipped");
        {
            TagOptions v3;
            v3.extendedHeader = true;
            v3.paddingBytes = 200;
            v3.major = 3;
            expectPicture (parse (buildTag ({ apic (png) }, v3)), png, "png", "v2.3 extended header + padding");

            TagOptions v4;
            v4.major = 4;
            v4.extendedHeader = true;
            v4.paddingBytes = 300;
            expectPicture (parse (buildTag ({ apic (png) }, v4)), png, "png", "v2.4 extended header + padding");
        }

        beginTest ("Compressed, encrypted, URL, and non-image pictures are skipped");
        {
            auto compressed = apic (png);
            compressed.flags = 0x0080;
            expect (! parse (buildTag ({ compressed }, {})).has_value());

            auto encrypted = apic (png);
            encrypted.flags = 0x0040;
            expect (! parse (buildTag ({ encrypted }, {})).has_value());

            Frame url;
            url.body.push_back (0);
            append (url.body, "-->");
            url.body.push_back (0);
            url.body.push_back (3);
            url.body.push_back (0);
            append (url.body, "http://example.invalid/x.png");
            expect (! parse (buildTag ({ url }, {})).has_value());

            const Bytes notAnImage { 'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd' };
            expect (! parse (buildTag ({ apic (notAnImage) }, {})).has_value());
            expect (! parse (buildTag ({ textFrame ("TIT2", "x") }, {})).has_value());
        }

        beginTest ("A grouping byte before the picture is skipped");
        {
            auto frame = apic (png);
            frame.body.insert (frame.body.begin(), 0x01);
            frame.flags = 0x0020;
            expectPicture (parse (buildTag ({ frame }, {})), png, "png", "grouping byte");
        }
    }

    void runRobustnessTests()
    {
        beginTest ("Truncated and corrupt tags never crash and never find a wrong picture");
        {
            const auto png = pngBytes();
            const auto tag = buildTag ({ textFrame ("TIT2", "Song"), apic (png) }, {});

            for (size_t length = 0; length <= tag.size(); ++length)
            {
                const auto found = findArtworkInId3 (tag.data(), length);

                if (length < tag.size())
                    expect (! found.has_value() || found->bytes.getSize() <= png.size());
            }

            // Bit flips in the header and frame headers.
            for (size_t index = 0; index < 40 && index < tag.size(); ++index)
            {
                auto corrupt = tag;
                corrupt[index] = static_cast<unsigned char> (corrupt[index] ^ 0xFF);
                findArtworkInId3 (corrupt.data(), corrupt.size());
            }

            auto lying = tag;
            lying[17] = 0x7F; // inflate the first frame size
            expect (! parse (lying).has_value());
            expect (! findArtworkInId3 (nullptr, 0).has_value());
            expect (! findArtworkInId3 ("ID3", 3).has_value());
            expect (! findArtworkInId3 ("not a tag at all", 16).has_value());
        }

        beginTest ("Image types are recognised by their first bytes");
        {
            expectEquals (sniffImageExtension (pngBytes().data(), pngBytes().size()), juce::String ("png"));
            expectEquals (sniffImageExtension (jpegBytes().data(), jpegBytes().size()), juce::String ("jpg"));
            expectEquals (sniffImageExtension ("GIF89a....", 10), juce::String ("gif"));
            expect (sniffImageExtension ("RIFF", 4).isEmpty());
            expect (sniffImageExtension ("", 0).isEmpty());
        }
    }

    juce::File writeFile (const test::ScopedTempDirectory& temp, const juce::String& fileName, const Bytes& bytes)
    {
        const auto file = temp.child (fileName);
        expect (file.replaceWithData (bytes.data(), bytes.size()));
        return file;
    }

    static Bytes wavWith (const std::vector<std::pair<juce::String, Bytes>>& chunks)
    {
        Bytes body;
        append (body, "WAVE");

        for (const auto& [id, data] : chunks)
        {
            append (body, id.toRawUTF8(), 4);

            for (int shift = 0; shift < 32; shift += 8)
                body.push_back (static_cast<unsigned char> ((data.size() >> shift) & 0xFF));

            append (body, data.data(), data.size());

            if (data.size() % 2 != 0)
                body.push_back (0);
        }

        Bytes wav;
        append (wav, "RIFF");

        for (int shift = 0; shift < 32; shift += 8)
            wav.push_back (static_cast<unsigned char> ((body.size() >> shift) & 0xFF));

        append (wav, body.data(), body.size());
        return wav;
    }

    void runFileTests()
    {
        const test::ScopedTempDirectory temp;
        const auto png = pngBytes();
        const auto tag = buildTag ({ textFrame ("TIT2", "Song"), apic (png) }, {});

        beginTest ("MP3 cover art is read from the ID3v2 tag at the start of the file");
        {
            auto bytes = tag;
            bytes.resize (bytes.size() + 4000, 0xFF); // stand-in for audio frames
            expectPicture (extractEmbeddedArtwork (writeFile (temp, "song.mp3", bytes)), png, "png", "mp3");
            expect (! extractEmbeddedArtwork (writeFile (temp, "plain.mp3", Bytes (500, 0xFF))).has_value());
            expect (! extractEmbeddedArtwork (writeFile (temp, "tiny.mp3", Bytes { 'I', 'D' })).has_value());
            expect (! extractEmbeddedArtwork (temp.child ("missing.mp3")).has_value());
        }

        beginTest ("WAV cover art is read from an id3 or ID3 chunk, even after the audio");
        {
            const Bytes fmt (16, 0);
            const Bytes audio (1001, 7); // odd length: exercises chunk padding

            for (const juce::String id : { "id3 ", "ID3 " })
            {
                const auto wav = wavWith ({ { "fmt ", fmt }, { "data", audio }, { id, tag } });
                expectPicture (extractEmbeddedArtwork (writeFile (temp, "tagged.wav", wav)), png, "png", id);
            }

            expect (! extractEmbeddedArtwork (
                          writeFile (temp, "untagged.wav", wavWith ({ { "fmt ", fmt }, { "data", audio } })))
                          .has_value());
            expect (! extractEmbeddedArtwork (writeFile (temp, "other.flac", tag)).has_value());

            auto lying = wavWith ({ { "fmt ", fmt }, { "id3 ", tag } });
            lying.resize (lying.size() - 20); // chunk extends past the end of the file
            expect (! extractEmbeddedArtwork (writeFile (temp, "cut.wav", lying)).has_value());
        }
    }

    void runFallbackAndChoiceTests()
    {
        beginTest ("The default artwork is deterministic per song and differs between songs");
        {
            const auto a1 = encodePng (makeFallbackArtwork ("Alpha.mp3"));
            const auto a2 = encodePng (makeFallbackArtwork ("Alpha.mp3"));
            const auto b = encodePng (makeFallbackArtwork ("Beta.mp3"));
            expect (a1 == a2);
            expect (! (a1 == b));

            const auto image = makeFallbackArtwork ("Alpha.mp3", 128);
            expectEquals (image.getWidth(), 128);
            expectEquals (image.getHeight(), 128);
            expect (image.getPixelAt (0, 0).getAlpha() == 255, "fully opaque");

            const auto decoded = juce::ImageFileFormat::loadFrom (a1.getData(), a1.getSize());
            expect (decoded.isValid());
            expectEquals (decoded.getWidth(), 512);
        }

        beginTest ("Choices carry their origin and are written into the job folder");
        {
            const test::ScopedTempDirectory temp;
            const auto fallback = makeFallbackChoice ("Song.wav");
            expectEquals (fallback.originId(), juce::String ("fallback"));
            expectEquals (fallback.image.extension, juce::String ("png"));

            const auto written = writeArtworkToJobDir (fallback, temp.child ("jobs\\job1"));
            expect (written.existsAsFile());
            expectEquals (written.getFileName(), juce::String ("artwork.png"));
            expect (written.getSize() == static_cast<juce::int64> (fallback.image.bytes.getSize()));

            ImageData embedded;
            const auto jpg = jpegBytes();
            embedded.bytes.append (jpg.data(), jpg.size());
            embedded.extension = "jpg";
            const auto choice = makeEmbeddedChoice (embedded);
            expectEquals (choice.originId(), juce::String ("embedded"));
            expectEquals (writeArtworkToJobDir (choice, temp.child ("jobs\\job2")).getFileName(),
                          juce::String ("artwork.jpg"));
        }

        beginTest ("A picture the user chooses is validated");
        {
            const test::ScopedTempDirectory temp;
            juce::String error;

            const auto png = pngBytes();
            const auto good = writeFile (temp, "cover.png", png);
            const auto choice = loadArtworkFile (good, error);
            expect (choice.has_value());

            if (choice)
            {
                expectEquals (choice->originId(), juce::String ("file"));
                expectEquals (choice->image.extension, juce::String ("png"));
            }

            // A JPEG saved with the wrong extension is still a JPEG.
            const auto mislabelled = loadArtworkFile (writeFile (temp, "photo.png", jpegBytes()), error);
            expect (mislabelled.has_value() && mislabelled->image.extension == "jpg");

            expect (! loadArtworkFile (
                writeFile (temp, "notes.png", Bytes { 'n', 'o', 't', ' ', 'a', 'n', ' ', 'i', 'm', 'g' }), error));
            expect (error.contains ("not a PNG, JPEG, or GIF"), error);

            auto truncated = png;
            truncated.resize (30);
            expect (! loadArtworkFile (writeFile (temp, "cut.png", truncated), error));

            expect (! loadArtworkFile (temp.child ("gone.png"), error));
            expect (error.contains ("not found"), error);
        }
    }
};

ArtworkTests artworkTests;
} // namespace
} // namespace snds
