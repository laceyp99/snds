# Diagnostics

When something fails on your machine, a diagnostics report lets someone else see the same
environment you do. There are two ways to make one. Both are read-only, both redact your user
folder, and **neither sends anything anywhere**. You decide whether to share the result.

| Use | When |
|---|---|
| `scripts\diagnostics.ps1` | Setup, bootstrap, build, or "worker not ready" problems, and anything before the app starts. |
| **Export diagnostics...** in the app | Problems while using SNDS (import, preparation, playback, devices, graphics). Includes the logs. |

For an environment issue, please include the script output. For an app issue, include the zip.

## The report script

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\diagnostics.ps1 > diagnostics.txt
```

Run it from the repository root. It always finishes: a section that fails reports its own error
instead of stopping the script.

The sections, in order:

| Section | What it shows |
|---|---|
| Source | The git commit and how many files are uncommitted. |
| System | Windows version, CPU, RAM, every graphics adapter with its Windows driver version (for an NVIDIA 560.94 driver that reads `32.0.15.6094`), and the `nvidia-smi` line if there is an NVIDIA GPU. |
| Toolchain | Versions of uv, CMake, Visual Studio and the MSVC toolset, and PowerShell. |
| Data directories | The SNDS data folder and whether `SNDS_HOME` is set. |
| Model | Whether the `htdemucs` checkpoint is present and verified. |
| Worker environment | The worker's readiness check: Python, torch build, CUDA state, protocol version. |
| Settings | **Use the CPU only**, whether an output device was chosen, and the recent songs. |
| Cache | Cached songs by key (first 12 characters), size, and status. |
| Logs | The log file names and sizes, and the last 20 lines of `app.log`. |
| App build | Whether the Debug and Release app and test executables are built. |

## Export diagnostics... in the app

Press **Export diagnostics...** in the top bar. The same button appears as an action on failures
that offer logs (a worker crash, a separation or validation error, an internal error, a song that
cannot be loaded). A "Save diagnostics" dialog suggests `snds-diagnostics-<date>-<time>.zip` in
your Documents folder. SNDS writes it in the background, shows it in Explorer, and a notice says
"Diagnostics saved to <path>."

The zip has:

- `diagnostics.txt`, with the same `##` sections as the script (System, Data directories, Model,
  Worker environment, Settings, Cache, Logs) plus three that need the running app:
  - **Application**: SNDS and JUCE versions, the build, and the worker folder.
  - **Renderer**: what the visuals are using (GPU or fallback) and why.
  - **Audio device**: the state, device names, sample rate and buffer size.
- `logs\`: the last 256 KB of each log file in the data folder, starting at a line boundary.

For automation, `SNDS.exe --export-diagnostics <file.zip>` writes the same zip after the first
readiness check and quits, without a dialog.

## What is redacted, and what is not

**Redacted.** Every line and every log passes through the same rule: your user profile folder
reads `%USERPROFILE%` and your local app data folder reads `%LOCALAPPDATA%`. The computer name and
user name are never collected.

**Not collected.** Song audio, stems, artwork, and cache files. Settings come from known fields
only, so an unknown field is never copied (the script lists other keys by name, without values).
The cache section holds only keys, sizes, status and file names.

**Still present, so look before you send.**

- The list of recent songs has each song's **title and the path of the original file**. Only the
  profile and local app data prefixes are replaced, so a path such as `D:\Music\Artist\Song.wav`
  stays as it is.
- Logs name the files SNDS worked on, and error messages can quote them.
- The report names your GPU, driver, CPU and installed tools.

Open `diagnostics.txt` (or the zip) and edit or remove anything you do not want to share. A
song title in a bug report is usually not needed; replace it if the file name is private.

## How to send it

1. Run the script or export the zip. Open it and read it once.
2. Attach it to the bug report (an issue or a message to the person who gave you SNDS), together
   with what you did, what you expected, and what happened. Mention the song's format and length
   if the problem is about one file.
3. Do not post it publicly if you are not comfortable with the items above.

## Logs on disk

The logs are in `%LOCALAPPDATA%\snds\logs` (or `SNDS_HOME\logs`): `app.log` for the app and
`worker.log` for the worker.
Failures also offer **Open logs folder**. A job also writes the worker's error output to
`worker-stderr.log` in its folder under `jobs\`.
