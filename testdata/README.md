# Test data

Fixtures for the C++ and worker tests. Two kinds live here:

- **Synthetic** files that are generated from code and contain no copyrighted material.
- **One owner-supplied music excerpt**, with its own licence terms (below).

| File | What | Source |
|---|---|---|
| `tone-stereo-44100.mp3` | 0.5 s stereo, 44.1 kHz, two sine tones, ~5 KB | `generate_mp3_fixture.py` |
| `asking42much-excerpt.wav` | 10.000 s of real music, stereo, 44.1 kHz, PCM16, 1,764,044 bytes | `generate_asking42much_excerpt.py` |
| `asking42much-excerpt.mp3` | the same 10 s encoded by libsndfile/LAME (about 190 kbps average, Xing/LAME tag), 239,847 bytes | `generate_asking42much_excerpt.py` |

## The music excerpt (`asking42much-excerpt.*`)

- **Source:** "asking42much master v2.wav", a stereo 44.1 kHz PCM16 mix, 2:35, supplied by its owner,
  Patrick Lacey.
- **Range:** 24.0 s to 34.0 s of the track (441,000 frames), a busy section. The WAV is an exact sample
  copy (no fades, no processing). The MP3 is encoded from those same samples.
- **Licence:** Owner-supplied by Patrick Lacey for SNDS testing only; all rights reserved; not for
  redistribution outside this repository.
- **Only the excerpt is committed.** The full track is never stored in the repository, in caches or in
  release archives; `generate_asking42much_excerpt.py` needs the original on hand and is only for
  regenerating these two files.
- **Pinned bytes (SHA-256):**
  - WAV `5b859d9c2a3e321b5470aa5959283ec3c8c15e2426c5f650d0a8d4d422d267aa`
  - MP3 `d8eeb337575c78ed99c3bfbf20c09aeb64d0e2a2d88105663465dc0b12902752`
- **What uses it:** `tests/cpp/FixtureTests.cpp` (import, MP3 encoder-delay accounting, sample-exact
  alignment against the WAV on real music, `planOriginal`) and `worker/tests/test_fixture_excerpt.py`
  (the real worker process with the fake separator on both files). A real-model run on the excerpt is
  opt-in: see `docs/testing.md`.
- **MP3 numbers (JUCE 8.0.15, libsndfile 1.2.0):** libsndfile reports 441,000 frames (it applies the
  LAME tag); JUCE decodes 442,368 frames (384 MPEG frames of 1152). The tag says encoder delay 576 and
  padding 792, so JUCE's decode starts 1105 frames (576 + 529) before the worker's.

Regenerate from the repository root (the worker environment has `numpy` and `soundfile`):

```
worker\.venv\Scripts\python.exe -I testdata\generate_asking42much_excerpt.py "<path to the original track>"
```

The generator exposes only libsndfile's quality knob, so the MP3 is an average-bitrate encode, not
constant bitrate. A different libsndfile/LAME build may produce different MP3 bytes; the tests assert
properties and the pinned hashes above, so regenerating means updating the hashes on purpose.

Regenerate the MP3 from the repository root (read-only use of the worker environment; needs
`numpy` and `soundfile` with libsndfile >= 1.1):

```
worker\.venv\Scripts\python.exe -I testdata\generate_mp3_fixture.py
```

The MP3 is an encoder output, so regenerating it may change its bytes slightly with the
libsndfile/LAME version; tests assert properties (format, rate, channels, approximate length),
never the exact bytes. WAV fixtures are generated in memory by the tests.

`tests/cpp/SongImportTests.cpp` records how many frames JUCE's MP3 decoder yields for this file
against libsndfile's count (22050): MP3 encoder delay and padding decide A/B alignment, see
`spikes/normalized-stems/README.md`.

Observed (JUCE 8.0.15, libsndfile 1.2.0): JUCE decodes this MP3 to 24192 frames (21 MPEG frames,
no LAME gapless trim); libsndfile reports 22050. The 2142-frame difference is encoder delay plus
padding. The worker's stems are aligned to the libsndfile count, so JUCE-decoded playback of the
original must trim the same delay (Phase 3+).
