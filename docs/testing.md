# Testing

SNDS has automated suites that run without the model, opt-in suites that use the real worker,
GPU, or audio device, and a manual checklist for the things only a person can judge. This page
lists them, records what was measured, and keeps the validation results by phase.

## Run the automated suites

```powershell
.\scripts\test.ps1                    # build (Debug) and run the C++ and Python tests
.\scripts\test.ps1 -Config Release    # the same in Release
.\scripts\test.ps1 -SkipPython        # C++ only        (-SkipCpp: Python only)
.\scripts\format.ps1 -Check           # clang-format and ruff, without changing files
```

| Suite | What it covers | Where |
|---|---|---|
| C++ (`snds_tests`, JUCE `UnitTestRunner`, category `snds`) | Import and fingerprint, cache and atomic publish, worker process and protocol, session state, playback, transport math, sample-rate conversion, the audio-thread contract, the DSP (filters, reverb, delay, limiter, meters), parameters and ephemeral mix, rendering tiers and fallbacks, error reports, recent songs, the cache view, diagnostics export, the fixture excerpt | `tests\cpp\` |
| Python (`pytest`) | Worker readiness, protocol and schema, decode, normalization, validation, BPM adapter, cancellation, atomic cache publish, and the whole worker as a subprocess with a **fake separator** (no model) | `worker\tests\` |
| Format and lint | `clang-format` over C++, `ruff` check and format over `worker\` | `scripts\format.ps1` |

Run one C++ group with `snds_tests.exe --filter <text>` (built under
`build\snds_tests_artefacts\<Config>\`). `SNDS_TEST_TIMES=1` lists the slowest tests at the end.
`scripts\test.ps1` runs the C++ suite as four shards in parallel processes (`ctest --parallel 4`,
`snds_tests.exe --shard <i>/4`); `snds_tests.exe` with no argument runs everything in one process,
which takes several times as long. `scripts\test.ps1` builds Debug first (the Release build is not
reused), so a first run in a fresh clone takes about 3.5 minutes, most of it the Python suite.

The fake separator (`SNDS_FAKE_SEPARATOR=1`) splits the mix into four stems that add back up to
it, so decoding, MP3 gapless trimming, publishing, and the package on disk are tested on real
music without the model. CI (`.github\workflows\windows.yml`) runs the format check, the Debug
suite, and the Release C++ suite on `windows-2022` with the CPU environment and no model download.

### Fixtures

`testdata\` holds a tiny synthetic MP3 and a 10 s excerpt of an owner-supplied track as WAV and
MP3. The excerpt's licence, source and checksums are in `testdata\README.md`. The full track is
never committed.

## Opt-in tests

These need something the normal suite does not assume. Each is skipped unless its variable is set.

| Variable | Enables | Notes |
|---|---|---|
| `SNDS_HEAVY_TESTS=1` | The full workloads of the slowest C++ tests: 300 reloads, all twelve source/device rate pairs, six alignment lags, 400 rapid seeks, all 256 solo combinations, a 20000-line worker pipe flood | The default suite runs reduced but complete versions |
| `SNDS_VALIDATE_LONG=1` | A 12-minute play-through, 400 seeks, 600 loop laps, 300 s of WAV stems on disk (about 30 s) | |
| `SNDS_VALIDATE_REAL=1` | The real owner-track package and its original, read-only | `SNDS_REAL_PACKAGE` and `SNDS_REAL_ORIGINAL` override the paths |
| `SNDS_REAL_DEVICE=1` | Opens the real default audio device and runs silently | |
| `SNDS_RENDER_GL_TESTS=1` | Real OpenGL context checks | Needs a GPU |
| `SNDS_RENDER_FULLSCREEN_TESTS=1` | A real fullscreen window (covers the display briefly) | |
| `SNDS_OVERSHOOT_PROBE=1` | Offline resampler diagnostics in `--filter "Visual validation: renderer failure and contention"` | Reports output peaks for bounded source samples, a polarity step and equal-rate bypass; deliberately constructed signals, no audio device |
| `SNDS_SNAPSHOT_DIR=<folder>` | Writes images of the stem cards with `--filter Phase4Snapshots` | |
| `SNDS_E2E_HOME=<folder>`, `SNDS_E2E_TRACK=<file>` | `snds_tests.exe --e2e`: the real worker and model end to end (import, prepare, cancel at each stage, reopen, missing original, Locate) | `SNDS_E2E_HOME` is a scratch data folder with both checkpoints under `models\` (the run `bootstrap.ps1` made with `SNDS_HOME` set to it); **also set `SNDS_HOME` to the same folder**, or the first check fails. The track is only read. `SNDS_E2E_TRACK=testdata\asking42much-excerpt.wav` runs it on the committed excerpt; the whole `--e2e` run takes about a minute on the development laptop's GPU (the excerpt itself prepares in 5 s on the GPU, 28 s on the CPU) |
| `SNDS_E2E_CUDA=1` | The GPU part of `--e2e`: detection, GPU separation, forced CPU, an injected GPU failure and the explicit CPU retry | Needs a working NVIDIA GPU |
| `SNDS_RUN_MODEL_TESTS=1` | Python: the real `htdemucs` and `beat_this` checkpoints | Reads installed checkpoints under `SNDS_HOME\models`, defaulting to `%LOCALAPPDATA%\snds\models`. `SNDS_TEST_MODEL_DIR` overrides only the Demucs checkpoint folder |
| `SNDS_RUN_CUDA_TESTS=1` | Python: the real GPU readiness check | |

Real-model tests read `htdemucs\955717e8-8726e21a.th` and `beat_this\small0.ckpt`
under the models folder. An explicit opt-in fails if a required checkpoint is missing.
These tests only read installed checkpoints and write results in pytest temporary folders.
For other opt-in tests, use a scratch `SNDS_HOME` to keep writes out of `%LOCALAPPDATA%\snds`.

## Measured baselines

The reopen timings below predate the background source hash check. Reopening still needs no
separation worker, but identity verification now reads the full original before enabling A/B.

Measured on 2026-10-06 on one development laptop: Intel Core i5-13500H (12 cores, 16 threads),
32 GB RAM, NVIDIA RTX 4050 Laptop GPU (driver 560.94, CUDA 12.1), Windows 11. **This is not the
six-core, 16 GB machine the plan names as the CPU baseline**; that machine has not been measured,
so treat the CPU figures as a best case.

| What | Result |
|---|---|
| Prepare the owner track (2:35), CPU | about 63 s (about 2.4 times real time) |
| Prepare the owner track (2:35), CUDA | about 10 s (about 15 times real time) |
| Prepare the 10 s excerpt, CPU | 19 s (MP3 fixture) and 28 s (excerpt) |
| Reopen a prepared song | about 0.1 s, no worker |
| Cache size per song | about 5 GB per hour of audio (four float32 stereo stems, 1.4 MB per second); the 2:35 track is about 218 MB and the 10 s excerpt 14 MB |
| Worker environment, CPU | about 1.4 GB (`worker\.venv`, `-Compute cpu`) |
| Worker environment, CUDA | about 4.4 GB (`-Compute cuda`, includes the CUDA runtime) |
| Checkpoints | `htdemucs` 84 MB, `beat_this` about 8 MB |
| `bootstrap.ps1 -Compute cpu` on a clean clone | 90 s |
| `bootstrap.ps1` (automatic: CUDA) on a clean clone | 77 s |
| `build.ps1` on a clean clone (Debug, then Release) | 182 s each; the configure step is about 45 s of it |
| Python suite (`pytest`, 354 passed, 4 skipped) | 120 to 164 s |
| C++ suite (`snds_tests`, default workloads), Release, one process | 69 s, 566,760 checks, 0 failed |
| C++ suite, Debug, one process | 124 s on a busy machine, 566,773 checks, 0 failed |
| C++ suite, Debug, four parallel shards (`scripts\test.ps1`) | 21 s on a quiet machine, 36 to 40 s on a busy one |
| `scripts\test.ps1` on a clean clone (Debug build, C++ and Python) | 206 s |
| `format.ps1 -Check`, `mkdocs build --strict` | 7 s, 3 s |
| Disk after setup and both builds | `worker\.venv` 4.4 GB (CUDA), `build\` 3.3 GB, data folder 103 MB (checkpoints and two small songs) |

## Manual alpha matrix

Things a person has to judge. Run on a supported Windows 11 x64 machine with the Release build,
and record the result and the machine. "Evidence" says what has been done so far.

| Check | What to do | Evidence |
|---|---|---|
| Clean CPU setup | Follow the README on a clean CPU-only machine, reach playback | Clone, bootstrap, build and the e2e journey run on the development laptop (Phase 6). Not yet run on a separate clean machine |
| Clean CUDA setup | Same on a clean NVIDIA machine | GPU separation, forced CPU, and CPU retry run on the development laptop (Phase 6). Not yet run on a separate clean machine |
| Import formats | PCM, float, mono, stereo, high-rate WAV and an MP3 | Automated for the validator and the fixtures. Not yet done by hand |
| Artwork flow | Embedded MP3 art, manual replacement, no art (generated cover), readable palette | Not yet done by hand |
| Separation quality | Listen to several genres and note the `htdemucs` artifacts | Not yet done |
| Cancel at every stage | Cancel at each reported stage; no process left, no partial cache | Automated against the real worker (Phase 6); not inspected by hand |
| Long playback | Play and seek through a long track; watch drift, memory, disk, dropouts | Automated up to 12 minutes (`SNDS_VALIDATE_LONG`); not done by ear on a long song |
| Device matrix | Switch devices, sample rates and buffer sizes; unplug and replug | Automated with simulated devices; not done on real hardware |
| Mix stress | Move gain, pan, cuts, sends, tempo, delay division and A/B quickly | Automated (`Phase4Stress`); not done by hand |
| Visual stress | Full effects and fullscreen with forced fallbacks (`SNDS_RENDER=...`) while stressing audio | Automated; not done by eye |
| Cache recovery | Move or edit the original, locate it, corrupt a stem, clear the cache | Automated; not done by hand |
| Diagnostics review | Export a bundle, read it, check the redaction | Run once against the real app (Phase 6); the review for usefulness is open |

A box is only "done" when someone ran it. Until every row has a result, the alpha is not signed
off.

## Validation results by phase

### Phase 3: transport and playback (`tests/cpp/Phase3ValidationTests.cpp`)

Default suite (about 11 s): a 90 s song plays through bit-exact with A/B toggles; 60 seeks verify
every stem from the target frame; 50 loop laps and 120 pause/resume cycles stay in order; a
28-hour song (4.4 billion frames) seeks exactly; source 44.1/48/96/192 kHz x device 44.1/48/96 kHz
renders have the exact length, end at the song's last frame and match four analytic tones to
-109 dBFS or better (bypass paths -147 dBFS); ten device loss/return cycles (with seeks while the
device is away) resume bit-exactly; device rate changes keep the position and speed; reserved
memory is independent of length; 270 reloads leave memory, handles and threads flat; WAV stems of 8 s
and 40 s use the same private memory while playing.

Opt-in (`SNDS_VALIDATE_LONG=1`, `SNDS_VALIDATE_REAL=1`): see the table above. On the real owner
track, Stem Mix and Original both play the whole 2:35 track bit-exactly against direct file reads
(0 mismatches, 0 underruns, about 100x real time offline), the stem sum differs from the original
by -28.5 to -35.3 dB at six positions (Demucs model error; the measured lag is 0), the same at a
48 kHz device, and 40 random seeks into the real files are exact.

Those historical measurements predate the final device-output limiter. The current real-file
validator compares direct file reads with the expected transport fade and final limiter applied,
so loud input can be reduced while frame alignment is still checked exactly.

### Phase 4: the mix

- **Ephemeral mix.** `settings.json` keeps only `format`, `version`, `forceCpu`, `audioDevice` and
  `recentSongs`; `song.json` changes only in `bpm.userOverride`; and no parameter ID appears in any
  text file under the data folder (`EphemeralMix` tests).
- **`Phase4Stress`**: another thread writes every parameter (extremes, resets, `resetMix`
  transactions) while the real engine plays through the sample-rate converter and is sought,
  paused, stopped and looped: no NaN or infinity, the final device output stays at or below
  the limiter ceiling after conversion, meters stay finite and ordered, zero allocations in `process`.
- **`Device output ceiling`**: a polarity step that overshoots in the raw converter stays bounded
  through the engine for Stem Mix and Original. A/B switches, seeks, device-rate changes and mono
  callbacks retain the ceiling and stereo balance without callback allocations.
- **`Phase4SoloUi`**: mute and solo pressed on the stem cards, against four constant stems of
  different levels, give exactly `stemAudibility` in the real output (96 combinations; all 256
  with `SNDS_HEAVY_TESTS=1`); Reset mix clears them.

### Phase 5: artwork and fullscreen

The renderer ladder (GPU, CPU, static), capability detection, frame pacing, and the hung-driver
handling are covered by the `Render` and `Visual panel` groups; see
[the architecture notes](architecture.md).
Real-GPU and kiosk-window checks are opt-in (`SNDS_RENDER_GL_TESTS`,
`SNDS_RENDER_FULLSCREEN_TESTS`). Force a fallback by hand with `SNDS_RENDER` (a comma list; see the
architecture notes).

### Phase 6: the whole journey (2026-10-06)

Everything ran against the real worker with a temporary `SNDS_HOME`; nothing touched
`%LOCALAPPDATA%\snds`, nothing was played (`--no-audio`), and the owner track and its excerpt were
only read.

| Check | How | Result |
|---|---|---|
| Clean machine, CPU | `git clone` of `ba48041` into a temp folder, `scripts\bootstrap.ps1 -Compute cpu` (90 s), `scripts\build.ps1` (182 s), `scripts\diagnostics.ps1`, then `snds_tests --e2e` with a 10 s excerpt | Pass: readiness "Worker ready: CPU", MP3 fixture and excerpt prepared on the CPU (19 s and 28 s), cancel during CPU separation, reopen from the cache (0.1 s), missing original and Locate, 62 checks |
| GPU, forced CPU, GPU failure, CPU retry | `tests/cpp/Phase6JourneyTests.cpp` with `SNDS_E2E_CUDA=1` on an RTX 4050 (driver 560.94, CUDA 12.1) | Pass: readiness reports CUDA; separation on `cuda`; `forceCpu` gives `cpu`; an injected GPU error (the worker's `SNDS_FAKE_FAIL=cuda` hook) gives `cuda_failed` with Retry on CPU and no cache entry; `retryOnCpu` then prepares on `cpu` and leaves no job folder |
| Reopen | same file | Pass: a prepared song opens with no artwork question and no worker; the recent row is `ready`; after moving the original it opens from the cache (`originalMoved`); a same-size edited file is rejected with `locate_mismatch` and the real file accepted; an original edited in place is a new song (artwork question, old entry untouched) |
| Cancel at each stage | same file; cancel is sent when the stage is first reported | Pass for checking, artwork, decoding, separating (CPU), normalizing, tempo, validating: ended cancelled, no cache entry, no partial entry, job folder deleted. Finalizing finished before the cancel arrived (a complete song, never partial) |
| Diagnostics from the running app | `SNDS.exe --no-audio --open-song <owner track key> --export-diagnostics <zip>` with a temp home | Pass: exit 0; `diagnostics.txt` and `logs\app.log`; no user name, computer name, profile or local app data path appears (all read `%USERPROFILE%` or `%LOCALAPPDATA%`); worker, renderer (OpenGL 3.2 on the Intel iGPU, 20 fps), cache and recent songs sections are filled |

Problems the run found and fixed: `formatBytes` printed every digit of a size that was not a
whole number (`String (x, 0)` is not "zero decimals"), and a failed attempt's job folder stayed
behind after a successful retry. The excerpt used is `testdata\asking42much-excerpt.wav` (10 s);
the real track was not copied.

### Phase 7: the README followed from scratch (2026-10-07)

A fresh `git clone` of `85e19e4` into a temporary folder, a temporary `SNDS_HOME`, and only
`README.md`, `docs/setup.md`, `docs/user-guide.md`, `docs/diagnostics.md` and this page.

| Step | Result |
|---|---|
| `bootstrap.ps1` (automatic: CUDA on the RTX 4050) | 77 s; "The worker is ready." |
| `build.ps1 -Config Release` | 182 s |
| `diagnostics.ps1` | 8 s; every section in the order `diagnostics.md` lists |
| `test.ps1` | 206 s: C++ in four shards 21 s, 354 Python tests passed, 4 skipped |
| `snds_tests --e2e` on `testdata\asking42much-excerpt.wav` | 65 s, 150 checks; the excerpt prepared in 4.8 s on the GPU (`SNDS_E2E_CUDA` unset, so the GPU-only checks were skipped) |
| App with `--no-audio --open-song <excerpt>` | "Worker ready: CUDA on NVIDIA GeForce RTX 4050 Laptop GPU", "Ready: asking42much-excerpt.wav"; Song cache... lists "2 prepared songs use 14.2 MB in total. Temporary files: 0 B." |
| `--export-diagnostics` | 11 s; `diagnostics.txt`, `logs\app.log`, `logs\worker.log`; no user or computer name, profile or app data path |
| `format.ps1 -Check`, `mkdocs build --strict` | clean; 7 s and 3 s |

Not done through the window: choosing a file in the native **Import song...** dialog (it keeps the
last folder and file used by `SNDS.exe`, so it cannot be driven reliably by script), so the import
itself was covered by `--e2e`, which uses the same session code.
