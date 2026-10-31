# Setup

SNDS is built from source on Windows 11 x64. The setup is three scripts: `bootstrap.ps1` (Python
worker and models), `build.ps1` (the app), `run.ps1` (launch). You do not activate a Python
environment or call Demucs yourself.

## Prerequisites

| Need | Notes |
|---|---|
| Windows 11 x64 | The only supported platform. |
| Windows PowerShell 5.1 | Installed with Windows. The scripts target it. |
| Git | To clone the repository. |
| [uv](https://docs.astral.sh/uv/getting-started/installation/) | Installs the pinned Python 3.11 and the worker packages. Open a new terminal after installing so it is on `PATH`. |
| CMake 3.22 or newer | On `PATH`, or the copy that comes with Visual Studio (the "C++ CMake tools for Windows" component). |
| Visual Studio 2022 or Build Tools 2022 | With the "Desktop development with C++" workload (MSVC, Windows SDK). |
| Internet during setup | The first configure downloads JUCE 8.0.15; bootstrap downloads PyTorch, the `htdemucs` checkpoint and the beat tracking checkpoint. Afterwards SNDS runs offline. |

If PowerShell refuses to run a script, run it as
`powershell -NoProfile -ExecutionPolicy Bypass -File scripts\<name>.ps1`.

### Hardware and storage

- **CPU path:** any 64-bit CPU. The target baseline is six cores and 16 GB of RAM; the times in
  [measured baselines](testing.md#measured-baselines) come from a faster machine.
- **GPU path (optional):** an NVIDIA GPU with a Windows driver 527.41 or newer. SNDS installs the
  CUDA 12.1 build of PyTorch, which brings its own CUDA runtime; you do not install the CUDA toolkit.
  Only NVIDIA GPUs accelerate separation. AMD and Intel GPUs use the CPU.
- **Graphics:** the artwork effects use OpenGL 3.2 or newer. Without it SNDS falls back to a CPU
  renderer, then to static artwork (see [Known limitations](known-limitations.md)).
- **Disk:**

| What | Size |
|---|---|
| Worker environment, CPU build (`worker\.venv`) | about 1.4 GB |
| Worker environment, CUDA build | about 4.4 GB |
| `htdemucs` checkpoint | 84 MB |
| beat tracking checkpoint | about 8 MB |
| JUCE source and the build tree (`build\`) | about 3.3 GB with both Debug and Release built |
| Each prepared song | about 5 GB per hour of audio: four float32 stereo stems, 1.4 MB per second (2:35 is about 220 MB, a 10 s clip about 14 MB) |

The cache is never trimmed automatically; see [the song cache](user-guide.md#the-song-cache).

## 1. Get the code and the worker environment

```powershell
git clone <repository URL> snds
cd snds
.\scripts\bootstrap.ps1
```

`bootstrap.ps1` picks the PyTorch build for you: **CUDA** if `nvidia-smi` works, otherwise **CPU**.
It then syncs `worker\.venv` from the committed `worker\uv.lock` with a uv-managed Python 3.11,
downloads both checkpoints into the data folder (checking their SHA-256), and runs a worker
readiness check. It is safe to re-run; a verified checkpoint is not downloaded again.

### CPU setup

```powershell
.\scripts\bootstrap.ps1 -Compute cpu
```

This forces the CPU build of PyTorch, even on a machine with an NVIDIA GPU. It is the reference
path and the fallback for everything else. Expect roughly 90 seconds on a fast connection (the
automatic CUDA setup took 77 seconds on the same connection: 25 s to install the packages and the
rest to download the two checkpoints).

### CUDA setup

```powershell
.\scripts\bootstrap.ps1 -Compute cuda
```

The CUDA build was validated on an RTX 4050 laptop GPU (driver 560.94, CUDA 12.1). Things to know:

- At the start of the app and before each job, the worker runs a small GPU computation in a
  throwaway process. **Only a GPU that passes is used**; otherwise SNDS uses the CPU and tells you.
- A GPU error in the middle of a job (`cuda_failed`, `out_of_memory`) stops that job and offers
  **Retry on CPU**.
- **Use the CPU only** (a checkbox next to Import) forces the CPU for new jobs.
- Switching builds later means running `bootstrap.ps1` again with the other `-Compute` value. It
  re-syncs the same `worker\.venv`; the models are kept.
- `-SkipModel` sets up the environment without downloading the checkpoints (the CI uses it).

### If bootstrap fails

| Symptom | Likely cause and fix |
|---|---|
| `uv` not found | Install uv, then open a new terminal. |
| Download errors | Re-run; a partial checkpoint is discarded and fetched again. Check the proxy or firewall. |
| `Model download failed` with a size or SHA-256 mismatch | The download was not the pinned checkpoint (often a proxy or captive portal page). The bad file is discarded; re-run on a clean connection. |
| CUDA reported unavailable or unhealthy in the app | Update the NVIDIA driver to 527.41 or newer, then use **Re-check**. The CPU still works. |

When asking for help, attach the output of `.\scripts\diagnostics.ps1` (see
[Diagnostics](diagnostics.md)).

## 2. Build

```powershell
.\scripts\build.ps1 -Config Release
```

The first run configures `build\` (Visual Studio 2022, x64) and downloads JUCE, which takes about
45 seconds; the whole first build took about 3 minutes (182 s). `Release` is recommended for playing;
Debug (the default) is for development and runs slower. Debug and Release are separate builds, so
`scripts\test.ps1` (which uses Debug) compiles the app a second time.

Clone into an ordinary folder such as `C:\src\snds`. Under `%TEMP%`, MSBuild prints warning
`MSB8029` ("The Intermediate directory or Output directory cannot reside under the Temp directory");
the build still succeeds, but MSBuild warns because incremental builds are unreliable there.

## 3. Run

```powershell
.\scripts\run.ps1 -Config Release
```

The script waits until you close the window and then prints the exit code. Anything after the
script's own options goes to the app, for example `.\scripts\run.ps1 -Config Release --no-audio`
(never opens an audio device; handy on a machine without sound or for a first look).

Under the header buttons the window shows a worker status line. It reads "Checking the worker
environment..." and then "Worker ready: CPU (no usable NVIDIA GPU)" or "Worker ready: CUDA on
<GPU name>". If it says "The worker environment is not set up." or "...is incomplete.", or that a
model is not installed or is damaged, run `scripts\bootstrap.ps1` and press **Re-check**.
**Show details** has the full readiness report.

## Where SNDS keeps files

Everything lives under `%LOCALAPPDATA%\snds`. Set `SNDS_HOME` to use another folder.

| Folder or file | Contents |
|---|---|
| `models\` | The checkpoints (never in the repository). |
| `cache\` | One folder per prepared song: four stem WAVs, `artwork.*` and `song.json`. See [the song cache](user-guide.md#the-song-cache). |
| `jobs\` | Temporary working folders of running jobs; abandoned ones are cleaned at startup. |
| `logs\` | `app.log` (the app) and `worker.log` (the worker, when a command writes one). |
| `settings.json` | CPU-only choice, the audio output device, and up to 20 recent songs. No mix state is stored. |

Your original files are never copied or modified; SNDS refers to them by path and checks them by
size and SHA-256 content hash.

## Updating

Pull the new commit, then run `bootstrap.ps1` (it re-syncs the lockfile) and `build.ps1`. A song
prepared by an older, incompatible version shows as incompatible in **Song cache...** and can be
prepared again.
