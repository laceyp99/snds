# SNDS

SNDS is a local visual remix instrument for Windows 11 x64. You open a WAV or MP3, SNDS
separates it on your machine into vocals, drums, bass, and other with a pinned Demucs
`htdemucs` model, and you remix the stems while the artwork reacts to what you hear.

**Status: alpha, built from source.** Nothing is uploaded; separation runs on your own CPU or
NVIDIA GPU. Read [Known limitations](docs/known-limitations.md) before you share a bug report.

## Quick start

You need Windows 11 x64, Git, [uv](https://docs.astral.sh/uv/getting-started/installation/),
CMake 3.22 or newer, and Visual Studio 2022 (or Build Tools) with the "Desktop development with
C++" workload. Setup needs about 1.4 GB (CPU) or 4.4 GB (NVIDIA) of disk for Python packages, plus the
92 MB of model checkpoints, JUCE and about 3 GB for the build folder, and downloads them. Details are in
[Setup](docs/setup.md).

Run these in Windows PowerShell 5.1. Clone into an ordinary folder (not under `%TEMP%`):

```powershell
git clone <repository URL> snds
cd snds
.\scripts\bootstrap.ps1                 # worker environment (CUDA if an NVIDIA driver is found) + models, about 1.5 minutes
.\scripts\build.ps1 -Config Release     # configures build\ on first use, about 3 minutes
.\scripts\run.ps1 -Config Release       # launch the app (the script waits until you close the window)
```

When the window opens, its status line reads "Worker ready: CUDA on <GPU name>" or "Worker ready: CPU
(no usable NVIDIA GPU)". If it says anything else, see [Setup](docs/setup.md#3-run).

If PowerShell blocks scripts, run each one as
`powershell -NoProfile -ExecutionPolicy Bypass -File scripts\<name>.ps1`.

In the app: **Import song...**, choose a WAV or MP3, confirm the artwork, wait for preparation
(a 2:35 song took about a minute on one fast laptop CPU and about ten seconds on its NVIDIA GPU), then
press **Play**. The [User guide](docs/user-guide.md) covers the controls.

If something goes wrong, run `.\scripts\diagnostics.ps1` or use **Export diagnostics...** in the
app, and attach the result to your report. See [Diagnostics](docs/diagnostics.md).

## Other scripts

```powershell
.\scripts\test.ps1             # builds Debug, then runs the C++ and Python tests (about 3.5 minutes)
.\scripts\format.ps1           # clang-format + ruff; -Check verifies without changing files
```

## Documentation

| Page | What it covers |
|---|---|
| [Setup](docs/setup.md) | Prerequisites, CPU and CUDA bootstrap, build, data folders, troubleshooting |
| [User guide](docs/user-guide.md) | Import, artwork, waiting, playback, mixing, A/B, fullscreen, the song cache |
| [Diagnostics](docs/diagnostics.md) | The report script, the in-app export, what is redacted, how to send it |
| [Known limitations](docs/known-limitations.md) | What the alpha does not do |
| [Testing](docs/testing.md) | Automated suites, opt-in tests, manual alpha checklist, measured baselines |
| [Architecture](docs/architecture.md), [Worker protocol](docs/protocol.md) | Design and contracts |
| [Third-party notices](THIRD_PARTY_NOTICES.md) | Dependency and model licences and caveats |

To read these as a site: `uvx --from mkdocs==1.6.1 mkdocs serve`.

## License

SNDS-owned source code is released under the MIT License (see `LICENSE`). SNDS is built with
[JUCE](https://juce.com), which is licensed separately under AGPLv3 or the JUCE 8 licence.
The test fixture `testdata/asking42much-excerpt.*` is owner-supplied for SNDS testing only (see
`testdata/README.md`).
