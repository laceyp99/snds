"""Generates testdata/asking42much-excerpt.wav and .mp3: a short excerpt of an owner-supplied track.

The track itself ("asking42much master v2.wav", stereo 44.1 kHz PCM16, 2:35) is NOT in this
repository and must never be committed; only the excerpt is. The owner (Patrick Lacey) supplied it
for SNDS testing. Run this only to regenerate the fixtures, with the original on hand:

  worker\\.venv\\Scripts\\python.exe -I testdata\\generate_asking42much_excerpt.py "<path to the track>"

The path can also come from the SNDS_FIXTURE_SOURCE environment variable. Defaults: a 10 s excerpt
starting at 24.0 s (a busy section; the loudest window of the track is near 27 s).

- The WAV is an exact sample copy (PCM16, no processing, no fades).
- The MP3 is encoded from those same samples by libsndfile's MP3 writer (LAME), with the Xing/LAME
  header (so the encoder delay and padding are in the file, as the alignment code expects). The
  writer exposes a quality knob, not a bitrate mode, so the bitrate is an average of about 190 kbps
  (VBR/ABR), not constant. Encoder output may differ slightly across libsndfile/LAME versions; the
  tests assert properties (format, rate, channels, length relations, alignment), never exact MP3
  bytes. The committed bytes are pinned by the SHA-256 values in testdata/README.md and
  tests/cpp/FixtureTests.cpp.
"""

import hashlib
import os
import sys
from pathlib import Path

import numpy as np
import soundfile as sf
from soundfile import _ffi, _snd

START_SECONDS = 24.0
LENGTH_SECONDS = 10.0
MP3_QUALITY = (
    0.18  # libsndfile compression level: 0 is the largest, 1 the smallest (about 190 kbps here)
)
SFC_SET_COMPRESSION_LEVEL = 0x1301

source = Path(sys.argv[1] if len(sys.argv) > 1 else os.environ.get("SNDS_FIXTURE_SOURCE", ""))
if not source.is_file():
    sys.exit(
        "usage: generate_asking42much_excerpt.py <path to the original track> (or set SNDS_FIXTURE_SOURCE)"
    )

info = sf.info(source)
if (info.samplerate, info.channels, info.subtype) != (44100, 2, "PCM_16"):
    sys.exit(f"unexpected source format: {info.samplerate} Hz, {info.channels} ch, {info.subtype}")

rate = info.samplerate
frames = round(LENGTH_SECONDS * rate)
pcm, _ = sf.read(
    source, start=round(START_SECONDS * rate), frames=frames, dtype="int16", always_2d=True
)
if len(pcm) != frames:
    sys.exit("the track is shorter than the excerpt range")

out = Path(__file__).resolve().parent
wav_path = out / "asking42much-excerpt.wav"
mp3_path = out / "asking42much-excerpt.mp3"

sf.write(wav_path, pcm, rate, format="WAV", subtype="PCM_16")

with sf.SoundFile(
    mp3_path, "w", samplerate=rate, channels=2, format="MP3", subtype="MPEG_LAYER_III"
) as mp3:
    level = _ffi.new("double*", MP3_QUALITY)
    _snd.sf_command(mp3._file, SFC_SET_COMPRESSION_LEVEL, level, 8)
    mp3.write(pcm.astype(np.float32) / 32768.0)

for path in (wav_path, mp3_path):
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    details = sf.info(path)
    print(
        f"{path.name}: {path.stat().st_size} bytes, {details.frames} frames, {details.samplerate} Hz, "
        f"{details.channels} ch, sha256 {digest}"
    )
