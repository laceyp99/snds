"""Regenerates the synthetic MP3 import fixtures in testdata/import/.

Run from the repository root with the worker interpreter (no activation needed):

    worker\\.venv\\Scripts\\python.exe testdata\\make_fixtures.py

Everything is synthesised here: sine tones encoded with LAME (via the ``lameenc`` package, which writes
no Xing/LAME header, and via ``soundfile``/libsndfile, which does), small PNG images written with zlib,
and ID3v2 tags assembled byte by byte. The output is deterministic for the pinned worker environment,
so the fixtures are rights-clear and reproducible. WAV fixtures are not stored: the C++ tests
generate them at runtime.
"""

from __future__ import annotations

import io
import math
import struct
import sys
import zlib
from pathlib import Path

try:
    import lameenc
    import numpy as np
    import soundfile as sf
except ImportError:  # pragma: no cover - reported to the developer, not tested
    sys.exit("lameenc/numpy/soundfile missing; run with worker\\.venv\\Scripts\\python.exe")

OUTPUT_DIR = Path(__file__).resolve().parent / "import"


# --------------------------------------------------------------------------------------------------
# Audio


def sine_pcm16(sample_rate: int, channels: int, seconds: float, frequencies: list[float]) -> bytes:
    """Interleaved 16-bit little-endian sine tones at -12 dBFS with 10 ms fades (no clicks)."""
    frames = int(round(sample_rate * seconds))
    fade = max(1, sample_rate // 100)
    amplitude = 0.25 * 32767.0
    out = bytearray()
    for n in range(frames):
        gain = min(1.0, n / fade, (frames - 1 - n) / fade)
        for channel in range(channels):
            value = amplitude * gain * math.sin(2.0 * math.pi * frequencies[channel] * n / sample_rate)
            out += struct.pack("<h", int(round(value)))
    return bytes(out)


def encode_mp3(sample_rate: int, channels: int, bit_rate: int, pcm: bytes) -> bytes:
    encoder = lameenc.Encoder()
    encoder.set_bit_rate(bit_rate)
    encoder.set_in_sample_rate(sample_rate)
    encoder.set_out_sample_rate(sample_rate)
    encoder.set_channels(channels)
    encoder.set_quality(2)
    return bytes(encoder.encode(pcm) + encoder.flush())


def encode_mp3_with_lame_tag(sample_rate: int, channels: int, pcm: bytes) -> bytes:
    """libsndfile/LAME output: VBR with a leading Xing frame carrying the LAME gapless tag.

    The int16 samples are passed as float64: libsndfile 1.2.2 garbles int16 input to its MP3
    encoder, while float input round-trips (the values are still exactly the int16 signal).
    """
    samples = np.frombuffer(pcm, dtype="<i2").reshape(-1, channels).astype(np.float64) / 32768.0
    buffer = io.BytesIO()
    sf.write(buffer, samples, sample_rate, format="MP3", subtype="MPEG_LAYER_III", compression_level=0.5)
    return buffer.getvalue()


# --------------------------------------------------------------------------------------------------
# Images


def png_rgb(width: int, height: int, pixel) -> bytes:
    """Minimal truecolour PNG; pixel(x, y) returns an (r, g, b) tuple."""
    rows = bytearray()
    for y in range(height):
        rows.append(0)  # filter type: none
        for x in range(width):
            rows += bytes(pixel(x, y))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b"")


def front_cover_png() -> bytes:
    """48x32 diagonal teal-to-violet gradient (the image tests expect as the front cover)."""
    return png_rgb(48, 32, lambda x, y: (40 + 4 * x, 90 + 2 * y, 200 - 2 * x))


def other_picture_png() -> bytes:
    """16x16 flat orange image stored as picture type 0 ("other") before the front cover."""
    return png_rgb(16, 16, lambda x, y: (240, 120, 20))


# --------------------------------------------------------------------------------------------------
# ID3v2


def syncsafe(value: int) -> bytes:
    return bytes(((value >> 21) & 0x7F, (value >> 14) & 0x7F, (value >> 7) & 0x7F, value & 0x7F))


def unsynchronise(data: bytes) -> bytes:
    """ID3v2 unsynchronisation: insert 0x00 after 0xFF when followed by 0x00/0xE0+ or at the end."""
    out = bytearray()
    for index, byte in enumerate(data):
        out.append(byte)
        if byte == 0xFF:
            following = data[index + 1] if index + 1 < len(data) else None
            if following is None or following == 0x00 or following >= 0xE0:
                out.append(0x00)
    return bytes(out)


def text_frame_body(text: str) -> bytes:
    return b"\x00" + text.encode("latin-1")


def apic_body(mime: str, picture_type: int, description: str, image: bytes) -> bytes:
    return b"\x00" + mime.encode("latin-1") + b"\x00" + bytes((picture_type,)) + description.encode("latin-1") + b"\x00" + image


def id3v23_tag(frames: list[tuple[bytes, bytes]], padding: int = 64) -> bytes:
    body = b"".join(kind + struct.pack(">I", len(data)) + b"\x00\x00" + data for kind, data in frames)
    body += b"\x00" * padding
    return b"ID3\x03\x00\x00" + syncsafe(len(body)) + body


def id3v24_unsync_tag(frames: list[tuple[bytes, bytes]], padding: int = 32) -> bytes:
    """ID3v2.4 with an extended header and per-frame unsynchronisation plus data length indicators."""
    extended = syncsafe(6) + b"\x01\x00"  # size (includes itself), one flag byte, no flags set
    body = bytearray(extended)
    for kind, data in frames:
        stored = syncsafe(len(data)) + unsynchronise(data)  # data length indicator precedes the data
        body += kind + syncsafe(len(stored)) + b"\x00\x03" + stored  # flags: unsynchronised + DLI
    body += b"\x00" * padding
    flags = 0x80 | 0x40  # unsynchronisation (all frames) + extended header
    return b"ID3\x04\x00" + bytes((flags,)) + syncsafe(len(body)) + bytes(body)


# --------------------------------------------------------------------------------------------------


def main() -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    stereo = encode_mp3(44100, 2, 128, sine_pcm16(44100, 2, 1.0, [440.0, 660.0]))
    mono = encode_mp3(48000, 1, 64, sine_pcm16(48000, 1, 1.0, [330.0]))
    tagged = encode_mp3_with_lame_tag(48000, 2, sine_pcm16(48000, 2, 1.0, [220.0, 550.0]))
    mono_lsf = encode_mp3(22050, 1, 32, sine_pcm16(22050, 1, 0.5, [330.0]))

    title = (b"TIT2", text_frame_body("SNDS synthetic tone"))
    fixtures = {
        "tone-stereo-44k1.mp3": stereo,
        "tone-stereo-44k1-apic.mp3": id3v23_tag(
            [
                title,
                (b"APIC", apic_body("image/png", 0, "other", other_picture_png())),
                (b"APIC", apic_body("image/png", 3, "front", front_cover_png())),
            ]
        )
        + stereo,
        "tone-mono-48k-v24-unsync.mp3": id3v24_unsync_tag(
            [title, (b"APIC", apic_body("image/png", 3, "", front_cover_png()))]
        )
        + mono,
        "tone-stereo-48k-lametag.mp3": tagged,
        # MPEG-2 Layer III (22.05 kHz): SNDS rejects this variant, see src/app/SongImport.cpp.
        "tone-mono-22k05-mpeg2.mp3": mono_lsf,
    }

    for name, data in fixtures.items():
        path = OUTPUT_DIR / name
        path.write_bytes(data)
        print(f"{path.relative_to(OUTPUT_DIR.parent.parent)}  {len(data)} bytes")


if __name__ == "__main__":
    main()
