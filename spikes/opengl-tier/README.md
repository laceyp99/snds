# OpenGL tier spike

This disposable spike renders a small animated GLSL effect through
`juce::OpenGLContext`, detects capability failures, and falls back to cheaper
tiers while a message-thread `juce::Timer` keeps ticking. It also settles
`unknown-opengl-minimum`. It is not the Phase 1 renderer.

## What it does

- The window has an artwork view and a status bar. A 60 Hz `juce::Timer` on the
  message thread records every tick so gaps can be measured.
- At 0.5 s the app attaches an `OpenGLContext` to the artwork view with
  `setOpenGLVersionRequired(openGL3_2)` (core profile),
  `setComponentPaintingEnabled(false)`, and continuous repainting.
- It draws a full-screen quad with a GLSL 1.50 fragment shader. The shader
  applies zoom, an 8-tap glow, and a vignette, driven by a fixed 120 BPM pulse,
  over a generated 512x512 stand-in artwork texture.
- Fallback ladder: `gl`, then `cpu` (`paint()` with pulse-driven zoom,
  brightness, and glow), then `static` (the image without animation). The app
  only ever moves down the ladder.
- Failures it detects:
  - The native context was not created (`isAttached()` is false after
    `attachTo`).
  - The context is below the required version. The app checks the real
    `GL_MAJOR/MINOR_VERSION` and `GL_SHADING_LANGUAGE_VERSION` after creation.
  - The shader fails to compile or link. The `OpenGLShaderProgram` log is
    captured.
  - `GL_CONTEXT_LOST` or `GL_OUT_OF_MEMORY` is reported.
  - The watchdog sees no GL frame for 2 s.
  - The CPU tier goes over budget (average `paint()` above 8 ms over 30
    frames).
- Threading: the render thread never touches message-thread state. It reports
  through atomics and `MessageManager::callAsync`, using a `SafePointer` that is
  created on the message thread. Only the message thread detaches the context.
- Self-test mode writes a JSON report with GL strings, frames, the transitions,
  CPU paint cost, and timer gap statistics. It also saves PNGs: a `glReadPixels`
  frame, and a final snapshot for the CPU/static tiers. Then it quits with exit
  code 0 (all checks pass) or 1.

Two executables are built from the same sources. `SndsOpenGLTier` lets Windows
choose the GPU. `SndsOpenGLTierDgpu` exports `NvOptimusEnablement` and
`AmdPowerXpressRequestHighPerformance`. `dumpbin /exports` confirmed both
symbols are present.

## Switches

| Switch | Effect |
| --- | --- |
| `--self-test <s>` | Run for `<s>` seconds, write the report, exit 0/1 |
| `--report <path>` | Report path (default: `opengl-tier-report.json` next to the exe) |
| `--force-no-opengl` | Never attach a context; go straight to `cpu` |
| `--force-shader-failure` | Compile a fragment shader with a syntax error |
| `--force-context-loss [ms]` | Render thread reports a simulated loss after `ms` (default 1500) |
| `--force-cpu-failure` | Declare the CPU tier over budget after 30 frames, so it drops to `static` |
| `--force-gl-hang [ms]` | Diagnostic: block the render thread once for `ms` (default 4000) |
| `--request-gl default\|3.2\|4.1\|4.3` | Version passed to `setOpenGLVersionRequired` |
| `--require-gl <maj.min>` | Minimum accepted after creation (default 3.2, which implies GLSL 1.50) |
| `--expect-tier gl\|cpu\|static` | Override the self-test's expected final tier |

## Pins and toolchain

JUCE 8.0.15, using the same commit-archive URL and SHA-256 as
`spikes/juce-fetch` (copied so this spike stands alone). CMake 4.3.0, Visual
Studio 2022 17.9 (MSVC 19.39), Windows SDK 10.0.22621.0, C++20. In Debug,
`JUCE_DONT_ASSERT_ON_GLSL_COMPILE_ERROR=1` is required; otherwise JUCE
`jassert`s on the forced shader error.

## Reproduce

```powershell
$build = "$env:LOCALAPPDATA\snds-spikes\opengl-tier-build"
cmake -S .\spikes\opengl-tier -B $build -G "Visual Studio 17 2022" -A x64
cmake --build $build --config Release --parallel
powershell -NoProfile -ExecutionPolicy Bypass -File .\spikes\opengl-tier\run-self-tests.ps1 -Config Release
```

The script runs all nine scenarios for 6 s each, writes reports and PNGs to
`$build\reports\<Config>`, and exits with 1 if any gating scenario fails. To run
one scenario, or to watch the app interactively (no arguments):

```powershell
$exe = "$build\SndsOpenGLTier_artefacts\Release\SndsOpenGLTier.exe"
(Start-Process $exe -ArgumentList '--self-test', '6', '--report', "$build\reports\manual.json", '--force-shader-failure' -Wait -PassThru).ExitCode
Start-Process $exe
```

## Result

Run on 2026-10-03 on Windows 11 Home 10.0.26200: Intel Core i5-13500H, 32 GB,
Intel Iris Xe (driver 32.0.101.6733) plus NVIDIA RTX 4050 Laptop GPU (driver
560.94). Internal panel 1920x1200 at 144 Hz, 125% scaling. JUCE 8 used its
default Windows renderer, Direct2D, for all non-GL painting.

Builds from an empty directory: configure 48.7 s, Debug 27.5 s, and Release
68.3 s (two executables each, LTCG in Release), with 0 warnings. The suite was
run twice on Release and once on Debug with the final binaries. **All 8 gating
scenarios passed every time.**

### Driver-reported context

| Executable, request | Vendor | Renderer | `GL_VERSION` | `GL_SHADING_LANGUAGE_VERSION` | Profile | Max texture |
| --- | --- | --- | --- | --- | --- | --- |
| default, 3.2 | Intel | Intel(R) Iris(R) Xe Graphics | 3.2.0 - Build 32.0.101.6733 | 1.50 - Build 32.0.101.6733 | core | 16384 |
| default, `default` | Intel | Intel(R) Iris(R) Xe Graphics | 4.6.0 - Build 32.0.101.6733 | 4.60 - Build 32.0.101.6733 | compatibility | 16384 |
| dGPU export, 3.2 | NVIDIA Corporation | NVIDIA GeForce RTX 4050 Laptop GPU/PCIe/SSE2 | 3.2.0 NVIDIA 560.94 | 1.50 NVIDIA via Cg compiler | core | 32768 |

By default the app runs on the **Intel iGPU**; the NVIDIA GPU is used only with
the Optimus export. `GL_CONTEXT_FLAGS` is `0x8` (`NO_ERROR`) in Release and
`0x2` (`DEBUG`) in Debug.

### Scenarios

Ranges cover the three final runs. "Gap" is the largest interval between timer
ticks. A clean steady tick is about 18 ms (the timer runs at about 60 Hz).

| Scenario | Final tier | Result | Attach on message thread | Detach on message thread | Max timer gap in transition |
| --- | --- | --- | --- | --- | --- |
| normal (iGPU) | gl | about 143 fps (vsync), readback luma 0.385 on beat vs 0.32 between | 34-48 ms | n/a | n/a |
| normal-dgpu | gl | about 142 fps; render-thread frame gaps up to 91 ms | 136-149 ms | n/a | n/a |
| request-gl-default | gl | 4.6 compatibility, same shaders work | 31-36 ms | n/a | n/a |
| force-shader-failure | cpu | log: `ERROR: 0:8: '}' : syntax error syntax error` | 35-41 ms | 11-18 ms | 35-41 ms |
| force-no-opengl | cpu | no context attempted | n/a | n/a | 18-19 ms |
| force-context-loss | cpu | about 215 GL frames, then about 235 CPU frames | 39-45 ms | 17-19 ms | 19-29 ms |
| require-gl-9.9 | cpu | capability gate: "OpenGL 3.2 / GLSL 1.50 is below the required OpenGL 9.9" | 37-41 ms | 11-13 ms | 37-41 ms |
| force-cpu-failure | static | gl skipped, then cpu, then static after 30 CPU frames | n/a | n/a | 18-19 ms |
| force-gl-hang (informational) | cpu | watchdog fired after 2 s, then **detach blocked** | 40-49 ms | **2,005-2,025 ms** | **2,006-2,025 ms** |

In the shader-failure and capability-gate rows, the failure is detected right
after attach, so their transition gap includes the attach time.

Other measurements:

- The largest steady-state timer gap in any gating scenario was 18.3-21.0 ms.
  Outside the attach and transition windows, no gap exceeded 50 ms.
- A CPU-tier `paint()` averaged 0.26-0.36 ms in Release (worst 1.9 ms) and
  0.47-0.72 ms in Debug (worst 6.9 ms). This is message-thread time; Direct2D
  does the GPU work outside `paint()`.
- Earlier runs with the same tier logic also passed. In those, the NVIDIA attach
  took up to 237 ms.
- In a few runs the window took 1.4-1.6 s to appear (330-660 ms typically).
  This happens before any GL work and is excluded from tick counting.

## Findings

1. **Requesting 3.2 core is not a guarantee.** If
   `wglCreateContextAttribsARB` fails, JUCE (`juce_OpenGL_windows.h`) silently
   falls back to `wglCreateContext`, which gives a legacy context that can be
   "GDI Generic" 1.1. The real version must be checked after creation, as this
   spike does.
2. **Release contexts are `KHR_no_error`.** Outside Debug, JUCE sets
   `WGL_CONTEXT_OPENGL_NO_ERROR_ARB`, so `glGetError` reports nothing in
   Release. JUCE also does not request a robustness context, so a real
   `GL_CONTEXT_LOST`/TDR is not reported. Detection has to rely on version
   checks, shader logs, and a frame watchdog.
3. **Component painting must be disabled.** With JUCE's default
   (`setComponentPaintingEnabled(true)`), the render thread takes the
   MessageManager lock every frame to paint child components. The visual
   component should paint nothing itself, and UI should live in sibling
   components.
4. **`attachTo` blocks the message thread.** It creates the native child window
   and WGL context synchronously: about 31-65 ms on Iris Xe and 136-237 ms on
   the RTX 4050. Attach once (for example when the visual view opens), not
   repeatedly.
5. **`detach()` blocks the message thread until the render thread finishes its
   frame.** It then calls `openGLContextClosing()` on the message thread with
   the context current. This normally takes 11-19 ms, but there is **no upper
   bound if the render thread hangs** (2 s in the hang diagnostic). A hung
   driver would freeze the UI timer during fallback. Audio is unaffected because
   it runs on its own thread.
6. The hybrid laptop defaults to the iGPU. That gives the vsync rate (about
   143 fps) for this effect with fast attach. The dGPU adds attach cost and
   occasional render-thread hitches of up to 91 ms, likely from the Optimus copy.

## Recommended minimum tier

**OpenGL 3.2 core profile with GLSL 1.50 (`#version 150`).** Request it with
`setOpenGLVersionRequired(openGL3_2)`, then verify `GL_MAJOR/MINOR_VERSION >=
3.2` and `GL_SHADING_LANGUAGE_VERSION >= 1.50`. Write shaders in core style
(`in`/`out`, `texture()`, a bound VAO). Anything below that minimum uses the
CPU tier.

Rationale:

- 3.2 is the lowest version JUCE can request as a core profile on Windows. The
  enum offers `defaultGLVersion`, `openGL3_2`, `openGL4_1`, and `openGL4_3`.
- The planned effects (pulse, scale, glow, texture, brightness) need only
  texture sampling, a few taps, and uniforms. GLSL 1.50 covers that.
- Both GPUs here return exactly 3.2 core when asked. `default` returned 4.6
  compatibility on Intel; it works, but it would let deprecated fixed-function
  use creep in and makes the tier depend on each driver.
- A 4.x minimum would buy nothing for these effects and would exclude more old
  iGPUs. A higher optional tier can be added later behind the same check.
- Do not export `NvOptimusEnablement` by default. The iGPU already reaches
  vsync, attaches faster, and uses less power. Users can still choose the dGPU
  through Windows graphics settings.

## Remaining unknowns

- Real failure modes that can't be forced on this machine: actual context
  creation failure (Microsoft Basic Display Adapter, VMs, Remote Desktop), TDR
  or device removal, and real driver hangs. The hang diagnostic suggests a
  design for `implement-renderer-fallbacks`: when the watchdog fires, avoid
  `detach()` while the render thread is stuck. Instead shrink or move the GL
  component, show a CPU sibling, and detach later. This was not tested.
- The `cpu` tier actually runs on JUCE 8's Direct2D renderer, which is
  GPU-backed through D3D11. That is independent of the OpenGL driver, but not
  pure CPU. Whether a GL failure should also switch the peer to JUCE's software
  renderer is undecided.
- Not tested: older or other GPUs (for example Sandy Bridge Intel HD 2000/3000,
  which are expected to stop at OpenGL 3.1 on Windows, and AMD), multi-monitor
  or DPI changes, minimise/restore (the watchdog is suppressed while minimised),
  fullscreen, and re-attaching GL after a fallback.
- Why the window sometimes takes 1.4-1.6 s to appear. It is not GL-related.
