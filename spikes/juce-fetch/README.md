# JUCE FetchContent spike

This disposable spike proves that CMake can fetch a pinned JUCE revision through
`FetchContent` and build the target shape planned for `create-root-cmake`: a
JUCE GUI app plus a C++ test executable, without git submodules or a Visual
Studio developer shell. It is not the Phase 1 project.

## Pin

- JUCE 8.0.15, the latest 8.0.x tag (released 2026-07-21).
- Tag `8.0.15` is a lightweight tag on commit
  `91ad83ae34a81e0833b1a2b0866f54846370ae53` (checked with `git ls-remote`).
- Archive: `https://github.com/juce-framework/JUCE/archive/91ad83ae34a81e0833b1a2b0866f54846370ae53.tar.gz`
- SHA-256: `04f8d5055382582c757be9da069ea98338005f98248facd9c2804435ac853e70`
  (22,896,965 bytes). Two `curl` downloads and the CMake download all had this
  hash.

The pin is a commit archive URL plus `URL_HASH`, not a git clone. That means no
history, one 23 MB download, and CMake rejects any content change. GitHub does
not formally guarantee byte-stable generated archives. If the hash ever stops
matching, the fallback is `GIT_REPOSITORY` with `GIT_TAG <full sha>`.
`GIT_SHALLOW` only works with branch or tag names, so a SHA pin means a full
clone.

JUCE 9 also exists (9.0.0 on 2026-07-21, latest 9.0.3 on 2026-09-28). This spike
uses 8.0.x as briefed. Moving to JUCE 9 is a separate decision, and JUCE 9 has
its own EULA, which was not reviewed.

## Toolchain

CMake 4.3.0, Visual Studio Community 2022 17.9 (MSVC 19.39.33522, toolset
14.39.33519), Windows SDK 10.0.22621.0, generator `Visual Studio 17 2022 -A x64`.
C++20.

## CMake 4 compatibility

- JUCE 8.0.15 declares `cmake_minimum_required(VERSION 3.22)` in its root
  `CMakeLists.txt`, `JUCEUtils.cmake`, and `JUCEModuleSupport.cmake`. CMake 4
  dropped compatibility with versions before 3.5, but that doesn't affect JUCE 8.0.15.
  `CMAKE_POLICY_VERSION_MINIMUM` is **not** needed. Configure printed no
  warnings.
- The spike declares `cmake_minimum_required(VERSION 3.22...4.3)`. On CMake
  3.30 and later this enables CMP0168, so `FetchContent` downloads directly
  instead of through a sub-build.
- At configure time, JUCE builds its `juceaide` helper with a nested CMake
  configure and build that uses the same generator. This takes most of the
  configure time.
- Ninja was not tested because it needs `cl.exe` on `PATH`, which requires a
  VS developer environment.

## Targets

- `snds_juce_fetch_app` (`juce_add_gui_app`, `juce_gui_basics`). With
  `--smoke-exit` it opens its window and exits with 0 only if the window is
  showing and the compiled JUCE version matches the pin.
- `snds_juce_fetch_tests` (`juce_add_console_app`, `juce_core`,
  `juce::UnitTestRunner`), registered with CTest. It checks the compiled header
  version, the linked runtime version (`JUCE v8.0.15`), and a header-only
  function shared with the app.

## Reproduce

The build tree, downloaded archive, and extracted JUCE sources all stay outside
the repository:

```powershell
$build = "$env:LOCALAPPDATA\snds-spikes\juce-fetch-build"
Remove-Item -Recurse -Force $build -ErrorAction SilentlyContinue
cmake -S .\spikes\juce-fetch -B $build -G "Visual Studio 17 2022" -A x64
cmake --build $build --config Debug --parallel
cmake --build $build --config Release --parallel
ctest --test-dir $build -C Debug --output-on-failure
ctest --test-dir $build -C Release --output-on-failure
(Start-Process "$build\snds_juce_fetch_app_artefacts\Release\SNDS JUCE Fetch Spike.exe" -ArgumentList '--smoke-exit' -Wait -PassThru).ExitCode
```

## Result

Passed on 2026-10-03 on Windows 11 Home 10.0.26200 (Intel Core i5-13500H,
12 cores, 32 GB), starting from an empty build directory:

| Step | Result | Time |
| --- | --- | --- |
| Cold configure (download + extract + `juceaide`) | pass, 0 warnings | 42.9 s |
| Debug build (app + tests) | pass, 0 warnings | 13.0 s |
| Release build (app + tests, LTCG) | pass, 0 warnings | 26.9 s |
| Warm reconfigure | pass | 3.9 s |
| `ctest -C Debug` / `-C Release` | 1/1 passed each (`6 passed, 0 failed`) | 0.1 s |
| App `--smoke-exit`, Debug and Release | exit code 0 | under 1 s |

Disk use: 22.9 MB download (kept in `_deps/juce-tmp`), 82.6 MiB extracted
source (4,425 files), 118 MiB `_deps/juce-build` (mostly `juceaide`), and
752 MiB for the whole build tree after both configurations.

Each `juce_add_*` target compiles its own copy of the JUCE module sources,
because JUCE modules are CMake `INTERFACE` libraries. The real app and tests
will each compile JUCE unless the scaffold adds a shared static library. That
layout is untested here.

## JUCE licence facts (for `spike-license-notes`)

Sources: the fetched `LICENSE.md` (SHA-256
`2633539bb26d244f0966fbc4df59400ea99bdf575fa96291851c1c3ff3456146`) and the
JUCE 8 EULA at <https://juce.com/legal/juce-8-licence/>, both read on 2026-10-03.
This is a fact summary, not legal advice.

- JUCE modules are dual-licensed: AGPLv3, or the commercial JUCE 8 EULA.
- JUCE 8 EULA licence types:

  | Type | Annual revenue or funding limit | Price |
  | --- | --- | --- |
  | Starter | up to $20,000 | free |
  | Indie | up to $300,000 | $40/month or $800 perpetual |
  | Pro | no limit | $175/month or $3,500 perpetual |
  | Educational | no limit | free |

  For an individual, the limit counts all revenue from use of the framework,
  including donations, sponsorship, and advertising.
- EULA clause 2.3 forbids making JUCE subject to a licence that forces source
  disclosure or free redistribution of JUCE or combined software. MIT for
  SNDS-owned code does not do this. If SNDS were distributed under AGPLv3
  instead, the combined binary would have to meet AGPLv3.
- There is no splash-screen requirement. JUCE 8.0.15 prints a compiler warning
  that `JUCE_DISPLAY_SPLASH_SCREEN` is ignored, and the EULA has no splash
  clause.
- `LICENSE.md` lists the bundled third-party code. The parts likely relevant to
  SNDS are FLAC and Ogg Vorbis (BSD), GLEW/Mesa/Khronos in `juce_gl.h`
  (BSD/MIT), jpeglib (IJG), libpng and zlib (zlib), HarfBuzz (Old MIT), SheenBidi
  (Apache 2.0), and ASIO (proprietary/GPLv3, only if enabled). Notices should
  cover only the modules actually linked.

## Remaining unknowns

- Whether to use JUCE 8.0.x or 9.0.x, including a JUCE 9 EULA review.
- Long-term byte stability of the GitHub archive (the fallback is above).
- A shared JUCE static library for app and tests, to avoid compiling JUCE
  twice.
- Ninja/CI builds, and offline or cached configure with
  `FETCHCONTENT_SOURCE_DIR_JUCE` or `FETCHCONTENT_FULLY_DISCONNECTED`.
