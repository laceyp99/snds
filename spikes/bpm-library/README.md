# BPM library spike

This disposable spike compares lightweight local BPM analyzers for Windows
installation, broad-genre accuracy, half/double-time behaviour, runtime, and
licence metadata (`spike-bpm-library`, `unknown-bpm-dependency`). Every
candidate must install next to the pinned separation stack without changing
torch/torchaudio/demucs/numpy. It is not the Phase 2 BPM adapter.

## Files

- `probe_install.py`: for each candidate, creates a scratch uv project (base
  pins + candidate) under `%LOCALAPPDATA%\snds-spikes\bpm-probe-projects`,
  locks it, and syncs it into `%LOCALAPPDATA%\snds-spikes\bpm-probe-<name>`
  with `--locked`. Source builds of the added packages are forbidden
  (`--no-build-package`) to mimic an end-user machine without a compiler,
  with a second attempt that allows builds. The script verifies the locked
  separation pins and reports added packages, download size, and installed
  size.
- `pyproject.toml` / `uv.lock`: the comparison environment, i.e. the base
  pins plus the three wheel-installable candidates as dependency groups
  (`librosa`, `deeprhythm`, `beat-this`). The lock resolves torch 2.1.2+cpu,
  torchaudio 2.1.2+cpu, numpy 1.26.4, demucs 4.0.1, and soundfile 0.12.1
  unchanged (57 packages).
- `bpm_baseline.py`: self-contained NumPy estimator (band-wise spectral-flux
  onset envelope at 100 fps, autocorrelation, harmonic comb with a log-normal
  120 BPM prior, fine refinement), returning BPM, confidence, and half/double
  scores.
- `compare.py`: renders 54 deterministic synthetic clips into a temp dir and
  optionally adds read-only real audio plus 12 s excerpts. It runs each
  candidate in its own subprocess (load time measured separately) and prints
  tables. Model files go to `%LOCALAPPDATA%\snds-spikes\bpm-library-models`
  and their SHA-256 is printed.

## Reproduce

```powershell
# 1. Install probes (scratch environments outside the repo; deletes and recreates them).
$base = "$env:LOCALAPPDATA\snds-spikes\uv-cpu-clean\Scripts\python.exe"
& $base -X utf8 .\spikes\bpm-library\probe_install.py

# 2. Comparison environment from the committed lock.
$env:UV_PROJECT_ENVIRONMENT = "$env:LOCALAPPDATA\snds-spikes\bpm-library"
uv sync --project .\spikes\bpm-library --locked --python 3.11 --all-groups
Remove-Item Env:UV_PROJECT_ENVIRONMENT

# 3. Comparison (aubio/madmom reuse the probe environments built in step 1).
$python = "$env:LOCALAPPDATA\snds-spikes\bpm-library\Scripts\python.exe"
& $python -X utf8 .\spikes\bpm-library\compare.py `
  --python "aubio=$env:LOCALAPPDATA\snds-spikes\bpm-probe-aubio\Scripts\python.exe" `
  --python "madmom=$env:LOCALAPPDATA\snds-spikes\bpm-probe-madmom-git\Scripts\python.exe" `
  --candidates baseline,librosa_tempo,librosa_beat,deeprhythm,beat_this_small0,beat_this_final0,aubio,madmom
# Optional, owner-supplied rights-cleared audio (read in place, never copied):
#   add --real "C:\path\to\track.wav"
```

## Installation results (Windows 11 x64, CPython 3.11, 2026-10-03)

| Candidate | Version | Licence | Install beside pins | Added packages / download / installed | Notes |
|---|---|---|---|---|---|
| librosa | 0.11.0 (1.0.0 needs Python ≥ 3.12 and NumPy ≥ 2.1) | ISC; deps BSD/MIT/Apache, plus `soxr` LGPL-2.1+ and `certifi` MPL-2.0 | **Wheels only** | 20 / 87.5 MiB / 265 MiB (llvmlite 116, scipy 105, scikit-learn 26, numba 12) | numba JIT on first call (~6 s per process) |
| aubio | 0.4.9 (2019) | GPL-3.0-or-later | sdist only, so needs a C compiler (built here with VS 2022) | 1 / 0.5 / 0.4 MiB | Fails on machines without build tools |
| madmom (PyPI) | 0.16.1 (2018) | BSD-3 code; models CC BY-NC-SA 4.0 | **Lock failed** (sdist imports Cython without declaring it) | — | — |
| madmom (git) | 0.17.dev0 @ `27f032e8` | same | no wheel; Cython build with a compiler | 3 / 35 MiB / 135 MiB (scipy, mido) | Non-commercial model licence |
| essentia | 2.1b6.dev1389 | AGPL-3.0 | **Failed**: no Windows wheel and no sdist | — | — |
| deeprhythm | 0.0.13 | AGPL-3.0 | **Wheels only** | 22 / 87.5 MiB / 266 MiB (librosa stack + nnAudio) | Weights `deeprhythm-0.7.pth` (5.4 MB) downloaded from GitHub `main` at runtime by default |
| beat-this | 1.1.0 | MIT (code and published weights) | **Wheels only** | 3 / 0.2 MiB / 0.6 MiB (`rotary-embedding-torch` MIT, `soxr` LGPL-2.1+) | Reuses torch; checkpoint `small0` 8.1 MB / `final0` 81 MB from the JKU cloud |
| tempocnn | 0.0.8 | AGPL-3.0 | Lock resolves, but pulls in TensorFlow 2.17 | 68 / 200 MiB download | Excluded: a second deep-learning framework |
| BeatNet | 1.1.3 | (none stated) | **Lock failed**: `numba==0.54.1` requires NumPy < 1.21 | — | — |

The in-house baseline adds nothing (NumPy only).

## Accuracy material

No rights-cleared music existed when the matrix was designed, so `compare.py`
renders 54 deterministic 30 s, 44.1 kHz drum/bass clips: 9 tempos (70, 85,
96, 110, 120, 128, 140, 160, 174 BPM) × 6 styles (four-on-the-floor, rock
backbeat, half-time hip-hop, swung shuffle, syncopated breakbeat, sparse
ambient pulse). Hits have ±4 ms humanization. They are rendered to a temp dir
and never committed. Classification uses the MIREX Acc1 4 % tolerance against
the notated tempo (exact) or its ×0.5 (half) / ×2 (double).

The owner later supplied one rights-cleared track (stereo 44.1 kHz PCM16,
154.9 s). It was analysed read-only in full and as two 12 s excerpts (from
0 s and 60 s) written to the temp dir. **Its ground-truth BPM is unverified.**

## Results

Hardware: Intel Core i5-13500H (12 cores), 32 GB, Windows 11. Accuracy is
from run 2 (2026-10-03 18:36:25–18:45:40 -04:00). **Run 2 timings are
contended**: a concurrent MSVC build (`MSBuild.exe`, `vctip.exe`) was running
throughout. Run 1 (14:01–14:11, not probed for builds) produced nearly
identical classifications before the beat-interval precision fix
(beat_this `small0` scored 48/0/5/1). Its steady-state timings were within
about ±40 % of run 2 (e.g. beat_this `small0` 2.25, baseline 0.25, madmom
7.4 s per audio minute).

| Candidate | Exact | Half | Double | Wrong | Within 1 BPM | s per audio minute (contended) | Load s | First call s | Confidence |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| baseline (NumPy) | 30/54 | 17 | 5 | 2 | 30 | 0.37 | 0.00 | 0.11 | yes (normalized autocorrelation + half/double scores) |
| librosa `feature.tempo` | 26/54 | 7 | 6 | 15 | 20 | 0.73 | 0.01 | 5.78 | no (can be derived from the tempogram) |
| librosa `beat_track` (interval tempo) | 28/54 | 8 | 8 | 10 | 28 | 0.33 | 0.00 | 5.95 | derived (beat regularity) |
| deeprhythm | 35/54 | 12 | 6 | 1 | 35 | 0.14 | 4.09 | 1.38 | yes (class probability) |
| **beat_this `small0`** | **48/54** | 0 | 6 | **0** | **48** | 2.26 | 2.45 | 1.25 | derived (beat regularity) |
| beat_this `final0` | 45/54 | 3 | 5 | 1 | 45 | 3.74 | 2.34 | 1.69 | derived |
| aubio (re-run after a NaN fix) | 27/54 | 19 | 0 | 8 | 24 | 0.07 | 0.01 | 0.03 | yes (unbounded) |
| madmom (RNN + tempo) | 31/54 | 16 | 4 | 3 | 29 | 10.08 | 0.88 | 6.36 | yes |

Per style (exact/half/double/wrong):

| Style | baseline | librosa tempo | deeprhythm | beat_this small0 |
|---|---|---|---|---|
| four_on_floor | 6/2/1/0 | 5/2/2/0 | 6/2/1/0 | 8/0/1/0 |
| rock_backbeat | 6/2/1/0 | 6/2/1/0 | 6/2/1/0 | 9/0/0/0 |
| halftime_hiphop | 4/3/2/0 | 5/0/2/2 | 6/2/1/0 | 8/0/1/0 |
| swung_shuffle | 7/2/0/0 | 8/1/0/0 | 6/2/1/0 | 9/0/0/0 |
| breakbeat | 0/6/1/2 | 0/2/1/6 | 6/2/1/0 | 7/0/2/0 |
| ambient_pulse | 7/2/0/0 | 2/0/0/7 | 5/2/1/1 | 7/0/2/0 |

Observations:

- The prior-centred methods mostly halve 160 and 174 BPM: baseline and
  deeprhythm halved all 12 such clips (6 styles × 2 tempos), and librosa,
  aubio, and madmom halved most of them. beat_this `small0` got all 12 right,
  plus 7 of 9 breakbeats. Its only misses were six double-time answers at
  70–96 BPM.
- beat_this quantizes beat times to 20 ms. Taking the median interval alone
  scored 18/54 within 1 BPM in run 1. Averaging all intervals within 10 % of
  the median brought that to 48/54.
- librosa and deeprhythm pay a numba/librosa start-up cost in every new
  process (~6 s first call for librosa). For the per-job worker, beat_this's
  import cost is mostly torch, which the worker already loads for Demucs.
  deeprhythm needs at least 8 s of audio.

Owner-supplied real track (**ground truth unverified**):

| Input | baseline | librosa tempo | librosa beat | deeprhythm | beat_this small0 | beat_this final0 | aubio | madmom |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Full 154.9 s | 120.00 (c 0.52) | 120.19 | 120.30 | 89.00 (c 0.93) | 179.84 (c 0.62) | 179.94 | 90.07 | 89.55 |
| 12 s from 0 s | 89.93 (c 0.40) | 90.67 | 90.01 | 89.00 (c 1.00) | 179.38 (c 0.55) | 178.64 | 101.98 | 89.55 |
| 12 s from 60 s | 120.00 (c 0.62) | 120.19 | 119.89 | 89.00 (c 0.99) | 180.10 (c 0.60) | 180.00 | 90.08 | 181.82 |

Agreement: deeprhythm, aubio, and madmom report about 89–90 BPM, and beat_this
reports its exact double (~180). This holds for the full track and both
excerpts, apart from one aubio outlier and one madmom double. The baseline and
librosa report **120 BPM on the full track and the 60 s excerpt**, a 4:3
ratio of the ~90 consensus (the onset autocorrelation has near-equal peaks at
0.33, 0.50, and 0.67 s). If ~90/180 is right, that is a non-octave error that
half/double correction cannot repair. The baseline's confidence (0.52–0.62)
did not flag it. Short 12 s inputs gave the same answer as the full track for
deeprhythm and beat_this.

## Recommendation

Use **beat_this 1.1.0 with the `small0` checkpoint** as the BPM dependency:

- Install: three pure-Python or wheel packages (0.6 MiB). Torch is reused, so
  there is no compiler, TensorFlow, or numba/scipy stack. The separation pins
  are unchanged.
- Accuracy: best exact rate on the synthetic matrix (48/54, no wrong answers),
  correct at fast tempos and on breakbeats, and its real-track estimate is an
  octave of the consensus. Its errors are double-time (6/54 synthetic, ~180 on
  the real track), which the app's half/double correction fixes in one step.
- Runtime: ~2.3 s per audio minute on CPU (contended), so about 9 s for a
  4-minute song after a ~2.5 s load. CUDA should be faster but was not
  measured.
- Licence: MIT for code and weights. The upstream README warns that some
  training data had restrictive licences. Dependencies are MIT, plus `soxr`
  LGPL-2.1+ (dynamically linked wheel).
- Phase 2 adapter: tempo = 60 / mean of the inter-beat intervals within 10 %
  of the median. Confidence = share of intervals within 5 % of the median
  (0..1). Store the beats count, regularity, and ×0.5/×2 alternatives in the
  metadata; user correction overrides the number. Pin the checkpoint like
  `htdemucs`: download `small0.ckpt` from
  `https://cloud.cp.jku.at/public.php/dav/files/7ik4RrBKTS273gp/small0.ckpt`
  during setup, verify SHA-256
  `6074be2c4d490c5f6101fcc374a1ec72ae93456e23bb6019783b849f5dc7d47b`
  (8,451,101 bytes), and never auto-download at job time. Call `Audio2Beats`
  with a local `checkpoint_path`, so torch.hub never downloads anything.
- Keep `bpm_baseline.py` as a zero-dependency diagnostic or fallback (for
  example, if the checkpoint is missing). Do not use it as the primary
  estimator: it can lock onto 4:3 ratios on real music.

Rejected: librosa (265 MiB of extra packages, numba start-up, weaker accuracy,
4:3 error on the real track); deeprhythm (AGPL-3.0, librosa stack, weights
pulled from an unpinned GitHub branch, halves fast tempos); aubio (GPL,
needs a compiler); madmom (needs a compiler, non-commercial models, slowest);
essentia (no Windows wheels, AGPL); tempocnn (TensorFlow, AGPL); BeatNet
(unsatisfiable with NumPy 1.26).

**Pause condition not triggered:** three BPM dependencies install from
wheels beside the pinned stack without changing torch/torchaudio/demucs/numpy.

## Remaining unknowns

- Genre accuracy on real music is still unproven. Only one real track was
  tested, and its BPM is unverified. Synthetic patterns are a lower bar than
  real mixes. The owner should confirm this track's BPM, and more
  rights-cleared fixtures are needed.
- Whether analysing the separated drums or drums+bass stems improves
  accuracy (separation already runs first).
- beat_this on CUDA, and adding `beat-this` to the CUDA lock (only
  pure-Python wheels plus `soxr`; expected trivial, not tested).
- Timings on the six-core baseline CPU without a concurrent build.
- Whether to ship the `small0` checkpoint as a separate pinned model artifact
  (as with `htdemucs`) and record its notice in `spike-license-notes`.
