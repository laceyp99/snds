# Normalized stems spike

This disposable spike checks that Demucs `htdemucs` output can be converted to
aligned stereo WAV at the source sample rate, with no truncation, offset,
clipping, or drift (`spike-normalized-stems`). It also covers
`verify-spike-audio-alignment` and proposes the
`unknown-normalization-tolerance` policy. It is not the Phase 2 worker.

## Pipeline under test (`normalize.py`)

1. Decode with `soundfile.read(dtype="float32", always_2d=True)`. Reject more
   than two channels. For MP3, reject a decode whose length is inconsistent
   with the stream's MPEG frame count (see MP3 below).
2. Build the model input: mono is duplicated to stereo. Other rates are
   resampled to 44.1 kHz with `julius.resample_frac(..., full=True)` (julius is
   already a Demucs dependency). Rounding the length up means the model always
   sees the whole source.
3. Separate like the Demucs 4.0.1 CLI: `(x - mean) / std`, then
   `apply_model(split=True, overlap=0.25, shifts=0)`, then `* std + mean`.
   `std` is guarded for digital silence (the CLI would compute 0/0 = NaN).
   `random.seed(0)` makes `shifts > 0` deterministic.
4. Resample each stem back to the source rate one at a time (bounds memory),
   using `output_length = source frames`. Pad or trim only as a fallback; the
   pad/trim counts are reported.
5. Write `drums,bass,other,vocals` as stereo float32 WAV (`subtype="FLOAT"`)
   with `soundfile`. Do not apply gain or limiting. Do not use Demucs
   `save_audio`: its default `clip="rescale"` changes each stem's gain whenever
   its peak exceeds 1.0.

## Pins

Same environments as `uv-cpu-env` / `uv-cuda-env`: CPython 3.11.9,
torch/torchaudio 2.1.2 (+cpu / +cu121), demucs 4.0.1, numpy 1.26.4,
soundfile 0.12.1 (bundled libsndfile 1.2.0, which supports MP3 read/write via
mpg123/LAME), julius 0.2.8. Checkpoint `955717e8-8726e21a.th` from the
htdemucs-artifact spike. No new environment or dependency is needed.

## Reproduce

```powershell
$cpu = "$env:LOCALAPPDATA\snds-spikes\uv-cpu-clean\Scripts\python.exe"
$cuda = "$env:LOCALAPPDATA\snds-spikes\uv-cuda-clean\Scripts\python.exe"
Set-Location .\spikes\normalized-stems
& $cpu -X utf8 check.py --device cpu
& $cuda -X utf8 check.py --device cuda
# Optional real-music case: owner-supplied, read in place and never copied.
& $cuda -X utf8 check.py --device cuda --real "C:\path\to\rights-cleared-track.wav"
Set-Location ..\..
```

`--only <substring>...` filters cases, `--no-long` skips the 180 s case, and
`--keep` keeps the generated audio. All synthetic sources, excerpts, and stems
are written to a `snds-normalized-stems-*` temporary directory that is deleted
at exit. The script exits nonzero if any case fails.

Synthetic cases (deterministic seeds, 10–20 s, odd frame counts): 44.1, 48,
96, and 192 kHz × mono/stereo × PCM16/PCM24/float32 WAV (24 cases; float
sources are mastered to a 2.0 peak to exercise values above full scale).
Also: MP3 VBR 44.1 kHz stereo/mono and 48 kHz stereo; MP3 CBR 44.1/48 kHz;
CBR and VBR with the LAME Info/Xing frame disabled; digital silence;
`shifts=1`; and a 180 s 48 kHz PCM24 drift case. The material contains
irregular drum hits, bass, a pad, and a vibrato voice with different L/R
content, so lags, channel swaps, and drift are measurable.

Owner-supplied real case (the `--real` option): a rights-cleared stereo
44.1 kHz PCM16 master, 154.9 s (6,830,914 frames), run in full plus a 20 s
excerpt (from 60 s) resampled to 48 kHz PCM24 in the temp dir.

## Result

Hardware: Windows 11 x64, Intel Core i5-13500H (12 cores), 32 GB, NVIDIA
GeForce RTX 4050 Laptop GPU (driver 560.94). Date: 2026-10-03.

**All 36 cases passed on CUDA** (run 18:21:44–18:24:37 -04:00; no build
process was running at the start, and `MSBuild.exe` was seen at the end).
**All 36 cases passed on CPU** (run 18:24:44–18:36:14 -04:00, with a
concurrent MSVC build running throughout, so its timings are contended). An
earlier full CPU run (14:18–14:27 -04:00, not probed for builds) produced the
same frame and alignment results. Its only failures were two 192 kHz cases of
a per-stem diagnostic that was too strict, since corrected (see "Per-stem
lag" below).

Measured on every non-rejected case, CPU and CUDA alike:

- **Frames:** stem frame count == decoded source frame count, exactly. The
  pad/trim fallback was never used (0 frames in all cases).
- **Format:** sample rate == source rate; 2 channels; WAV `FLOAT`; all
  samples finite; read-back data bit-exact with the in-memory stems.
- **Lag:** the cross-correlation lag between the sum of stems and the source
  was 0 samples over the full file and in the first and last 3 s windows.
  The largest sub-sample estimate was 0.0029 samples.
- **Drift:** |end − start| ≤ 0.0083 samples (≤ 0.003 ppm), including the
  180 s synthetic case and the 154.9 s real track.
- **Resampling alone** (source → 44.1 kHz → source, no model): lag 0 at every
  rate; identity at 44.1 kHz. The band-limiting residual was −59 to −91 dB at
  48 kHz, −60 to −62 dB at 96 kHz, and −62 to −65 dB at 192 kHz (julius
  `rolloff=0.945` removes content above ~20.8 kHz).
- **Sum-of-stems residual vs source:** −35.4 to −38.6 dB on synthetic
  material; −29.1 dB on the real track and −28.7 dB on its 48 kHz excerpt.
  This is model error (Demucs stems do not sum exactly to the mixture), not
  misalignment.
- **Peaks and clipping:** values above 1.0 occur and are preserved bit-exact
  by float32 WAV. Synthetic float sources produced stem peaks up to 1.77
  (up to ~32k samples above 1.0 per case). **The real PCM16 master (source
  peak exactly 1.0) produced a vocals stem peaking at 1.126 (67 samples
  above 1.0); the stem sum peaked at 1.061.** Even a legal full-scale PCM
  master yields stems above 0 dBFS, so PCM16/24 stem storage would clip, and
  the Demucs CLI's default `rescale` would silently change gain.
- **Silence:** with the guard, the output is all zero and finite.
- **`shifts=1`:** Demucs' random-shift trick (seeded) stayed aligned (lag 0).
- **Per-stem lag** vs the known synthetic component (diagnostic): drums
  ≤ 0.0034 samples. The bass stem showed a model-side low-frequency phase
  offset of up to 0.165 samples at the 44.1 kHz model rate, present even at
  44.1 kHz input, which involves no resampling. At 192 kHz this reads as
  −0.58 source samples, which rounds to an integer lag of −1. The check
  therefore bounds per-stem lag at 0.25 model-rate samples; the
  sum-of-stems lag remains the hard gate.

Separation time (excluding model load; one warm-up call first):

| Input | CUDA (RTX 4050), 18:21 | CPU, 14:18 run (not probed) | CPU, 18:24 run (**contended**: MSBuild active) |
|---|---:|---:|---:|
| Real track, 154.9 s, 44.1 kHz stereo | 5.16 s (0.033× real time) | 59.5 s (0.38×) | 141.8 s (0.92×) |
| Synthetic 180 s, 48 kHz stereo | 6.07 s (0.034×) | 80.4 s (0.45×) | 90.9 s (0.50×) |
| Real excerpt, 20 s, 48 kHz | 0.78 s | 8.8 s | 37.8 s |

The 14:18 CPU figures match the htdemucs-artifact benchmark (0.48× real time
uncontended), so they are the better estimate. Treat the 18:24 column only as
evidence that a concurrent compiler build can more than double CPU
separation time. Resampling back to 192 kHz added under 0.6 s per 20 s case.

### MP3

libsndfile 1.2.0 honours the LAME Info/Xing gapless header: in all five MP3
cases (VBR and CBR, 44.1/48 kHz, mono/stereo), the decoded frame count
exactly equals the original PCM count, and the offset vs the original PCM is 0.

With the Info/Xing frame disabled, emulating an encoder that wrote no gapless
metadata or a decoder that ignores it:

- CBR: 664,704 frames decoded (+3,103), with the audio **delayed by 2,257
  samples** (51 ms; the 1,152-sample info frame plus 1,105 samples of
  encoder/decoder delay). `sf.info().frames` reported 666,228, so the header
  estimate is not authoritative.
- VBR: libsndfile stops at a length estimated from the first frame's bitrate
  and **silently truncates** (535,296 of ~664,704 frames). `normalize.py`
  walks the MPEG frame headers and rejects the decode as possibly truncated.

Stems are always aligned to the worker's decode (lag 0 in every case). The
risk is a different decoder: if the JUCE MP3 decoder does not apply the LAME
delay/padding (or truncates VBR differently), Original A/B would be offset by
~2,257 samples. The JUCE decoder must be checked against the same files in
Phase 3.

### Storage

Stereo float32 per stem: 21.2 MB/min at 44.1 kHz, 23.0 at 48 kHz, 46.1 at
96 kHz, and 92.2 at 192 kHz. All four stems of a 4-minute song take 0.34 GB at
44.1 kHz and 1.47 GB at 192 kHz. A single stem reaches the 4 GiB RIFF limit at
~46 minutes at 192 kHz, so longer inputs would need RF64 or a duration cap.
Peak worker memory grows with rate and length; per-stem resampling helps, but
long 192 kHz songs may need chunked resampling.

## Proposed tolerance and clipping policy (`unknown-normalization-tolerance`)

1. **Frame count:** exactly equal to the worker's decoded source frame count
   (tolerance 0). Use `full=True` forward and `output_length` backward. Any
   pad/trim beyond the fallback is a validation failure, not a fix-up.
2. **Format:** source sample rate, 2 channels (mono duplicated), WAV float32
   (`FLOAT`).
3. **Alignment gate:** the sum-of-stems vs source cross-correlation must have
   integer lag 0 in the first and last 3 s windows, with sub-sample drift
   |end − start| < 0.05 samples (measured max 0.0083). Per-stem lag is
   diagnostic only (≤ 0.25 model-rate samples).
4. **Clipping:** no clipping, limiting, normalization gain, or Demucs
   `prevent_clip`. Peaks above 1.0 are allowed and preserved. Record per-stem
   peaks in metadata and leave headroom to the mixer/limiter. Non-finite
   samples fail validation. Guard `std == 0` (silence).
5. **Residual:** log the sum-of-stems residual as a diagnostic (synthetic
   about −36 dB, real −29 dB). Do not gate on it until more real fixtures
   exist; a sanity warning around −20 dB is a reasonable starting point.
6. **MP3:** the alignment reference is the decoded sample stream with LAME
   gapless trimming. Reject decodes inconsistent with the MPEG frame count.
   Store the decoder identity and decoded frame count in provenance.

No pause condition was triggered: source-rate normalization is sample-exact
and drift-free on every tested rate, layout, and codec.

## Remaining unknowns

- The JUCE MP3 decoder's gapless and VBR-length behaviour against these
  files (Original A/B alignment), and JUCE's float WAV reader for stems.
- Separation quality and residual on more real genres. Only one real track
  was tested; its true stem content is unknown.
- Peak memory and runtime for long 96/192 kHz inputs on the six-core
  baseline CPU, and whether chunked resampling is needed.
- WAV > 4 GiB (RF64) handling, if long high-rate inputs are supported.
- MP3 files with ID3v2 tags, free-format, or MPEG-2/2.5 streams beyond the
  frame walker's basic coverage.
