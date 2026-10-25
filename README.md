# SNDS

A local visual stem-remix instrument for Windows. SNDS is being built to split a song into vocals, drums, bass, and other, then let you mix those stems alongside reactive artwork.

**Early development.** The current app includes setup checks, logging, and placeholder import/player screens. Song import, stem mixing, and reactive visuals are still in development. Separation and audio-analysis prototypes live in `spikes/`.

## Development

Requires **Windows 11 x64**, Visual Studio 2022 or Build Tools with **Desktop development with C++**, CMake 3.25+, Git, and [uv](https://docs.astral.sh/uv/). The app uses C++/JUCE; the preparation worker uses Python 3.12, PyTorch, and Demucs.

Run from the repository root in PowerShell:

```powershell
.\scripts\bootstrap.ps1 -Compute cpu
.\scripts\configure.ps1
.\scripts\build.ps1
.\scripts\run.ps1
```

Bootstrap installs the pinned Python environment. Use `-Compute cuda` for a supported NVIDIA GPU with an R570+ driver. Initial setup needs internet access to download dependencies.

```powershell
.\scripts\test.ps1
.\scripts\lint.ps1
.\scripts\diagnostics.ps1
```

See [foundation spike results](docs/spikes.md) for technical decisions, measurements, and known limitations.

CI builds the Debug app and C++ tests, runs C++ and Python tests, and checks formatting on Windows with CPU dependencies.

## License

SNDS-owned code is [MIT licensed](LICENSE). JUCE and other dependencies have separate terms; JUCE uses AGPLv3 or a commercial license.
