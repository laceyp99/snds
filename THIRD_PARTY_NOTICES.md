# Third-party notices

SNDS-owned source code is licensed under the MIT licence. SNDS depends on the
third-party software and models listed below. This repository does not
redistribute any of these components: JUCE is downloaded by CMake, the Python
packages are installed by `uv` from a committed lockfile, and the model
checkpoints are downloaded into `%LOCALAPPDATA%`.

This file records what SNDS uses and the caveats known for private prototype
sharing among technical testers. It is not legal advice, and it is not a
clearance for binary or commercial distribution. Revisit every entry before
publishing a binary, installer, or hosted service.

## Native application

| Component | Version | Licence | Notes |
| --- | --- | --- | --- |
| [JUCE](https://github.com/juce-framework/JUCE) | 8.0.15 (`91ad83ae34a81e0833b1a2b0866f54846370ae53`) | AGPLv3, or the JUCE 8 EULA | Pinned by URL and SHA-256 through CMake `FetchContent` |

JUCE is dual-licensed. Building SNDS from source for personal evaluation needs
no licence decision. A binary release must either satisfy AGPLv3 for the
combined application or use a JUCE 8 EULA tier (Starter is free up to $20,000
annual revenue or funding). EULA clause 2.3 forbids placing JUCE under a
licence that forces source disclosure; MIT for SNDS-owned code does not do
that. JUCE 8 requires no splash screen.

SNDS links these JUCE modules (see `CMakeLists.txt`): `juce_core`,
`juce_events`, `juce_data_structures`, `juce_graphics`, `juce_gui_basics`,
`juce_audio_basics`, `juce_audio_devices`, `juce_audio_formats`,
`juce_audio_utils`, `juce_dsp`, and `juce_opengl`. JUCE's `LICENSE.md` lists
the third-party code bundled in them. The entries that apply to this link set
are:

| Bundled code | Module | Licence |
| --- | --- | --- |
| FLAC | `juce_audio_formats` | BSD |
| Ogg Vorbis | `juce_audio_formats` | BSD |
| GLEW loader in `juce_gl.h`, including Mesa and Khronos headers | `juce_opengl` | BSD, MIT |
| jpeglib | `juce_graphics` | Independent JPEG Group |
| libpng | `juce_graphics` | zlib |
| zlib | `juce_core` | zlib |
| HarfBuzz | `juce_graphics` | Old MIT |
| SheenBidi | `juce_graphics` | Apache 2.0 |

Entries in that list for code SNDS does not link are not used: AudioUnitSDK,
Oboe, CHOC, LV2, pslextensions, AAX, VST3, Box2D, and ASIO (the ASIO and AAX
SDKs are also not enabled). The authoritative list is JUCE's `LICENSE.md` at
the pinned version.

MP3 playback uses JUCE's built-in `MP3AudioFormat` decoder, which is covered by
the JUCE licence and carries JUCE's patent disclaimer. The relevant MP3
patents have expired.

## Python worker

Versions are those resolved by `worker/uv.lock` (the worker project's lockfile),
checked on 2026-10-06 against the installed `worker/.venv`. Licences come from
each package's installed metadata. Refresh this table whenever the lockfile
changes. The lockfile targets Windows x64 and Python 3.11 only.

Direct dependencies:

| Package | Version | Licence | Role |
| --- | --- | --- | --- |
| demucs | 4.0.1 | MIT | Source separation (upstream repository is archived) |
| beat-this | 1.1.0 | MIT | Beat tracking for the global BPM estimate (chosen in `spikes/bpm-library/`) |
| torch | 2.1.2 (`+cpu` or `+cu121`) | BSD-3-Clause | Inference runtime |
| torchaudio | 2.1.2 (`+cpu` or `+cu121`) | BSD-2-Clause | Audio I/O and resampling helpers |
| numpy | 1.26.4 | BSD-3-Clause | Array processing |
| soundfile | 0.12.1 | BSD-3-Clause | WAV/MP3 reading and WAV writing |
| packaging | 25.0 | Apache-2.0 OR BSD-2-Clause | Torch version checks |

Transitive dependencies:

| Package | Version | Licence | Required by |
| --- | --- | --- | --- |
| rotary-embedding-torch | 0.8.9 | MIT | beat-this |
| soxr | 1.1.0 | LGPL-2.1-or-later | beat-this (resampling; dynamically linked wheel) |
| einops | 0.8.2 | MIT | beat-this, demucs, rotary-embedding-torch |
| julius | 0.2.8 | MIT | demucs (resampling) |
| openunmix | 1.3.0 | MIT | demucs |
| dora-search | 0.1.13 | MIT | demucs |
| lameenc | 1.8.4 | LGPL-3.0-or-later | demucs (MP3 export); not called by SNDS |
| omegaconf | 2.3.1 | BSD-3-Clause | dora-search |
| antlr4-python3-runtime | 4.9.3 | BSD-3-Clause | omegaconf |
| PyYAML | 6.0.3 | MIT | demucs, omegaconf |
| submitit | 1.5.4 | MIT | dora-search |
| cloudpickle | 3.1.2 | BSD-3-Clause | submitit |
| treetable | 0.2.6 | Unlicense | dora-search |
| retrying | 1.4.2 | Apache-2.0 | dora-search |
| tqdm | 4.70.1 | MPL-2.0 AND MIT | demucs, beat-this |
| filelock | 4.0.9 | MIT | torch |
| fsspec | 2026.9.0 | BSD-3-Clause | torch |
| Jinja2 | 3.1.6 | BSD-3-Clause | torch |
| MarkupSafe | 3.0.4 | BSD-3-Clause | Jinja2 |
| networkx | 3.6.1 | BSD-3-Clause | torch |
| sympy | 1.14.0 | BSD-3-Clause | torch |
| mpmath | 1.3.0 | BSD-3-Clause | sympy |
| typing-extensions | 4.16.0 | PSF-2.0 | torch |
| cffi | 2.1.1 | MIT-0 | soundfile |
| pycparser | 3.0 | BSD-3-Clause | cffi |
| colorama | 0.4.6 | BSD-3-Clause | tqdm (Windows) |

Development-only packages (the `dev` dependency group, used for tests and
linting, not needed to run the worker): pytest 9.1.1 (MIT), pluggy 1.6.0 (MIT),
iniconfig 2.3.0 (MIT), Pygments 2.21.0 (BSD-2-Clause), ruff 0.16.10 (MIT).

The soundfile wheel bundles `libsndfile` under LGPL-2.1-or-later. That build
includes the mpg123 decoder (LGPL-2.1) and LAME encoder (LGPL-2.0) for MP3,
plus FLAC, Ogg, Vorbis, and Opus libraries under BSD-style licences. SNDS loads
it dynamically through the unmodified wheel.

The CUDA builds of PyTorch bundle NVIDIA CUDA runtime libraries under NVIDIA's
redistribution terms. SNDS does not redistribute them; `uv` downloads them from
the PyTorch index on the user's machine.

## Model checkpoints

| Artifact | Identifier | SHA-256 | Source |
| --- | --- | --- | --- |
| `htdemucs` checkpoint | `955717e8-8726e21a.th` (84,141,911 bytes) | `8726e21a993978c7ba086d3872e7608d7d5bfca646ca4aca459ffda844faa8b4` | `https://dl.fbaipublicfiles.com/demucs/hybrid_transformer/` |
| beat_this `small0` checkpoint | `small0.ckpt` (about 8.1 MB) | `6074be2c4d490c5f6101fcc374a1ec72ae93456e23bb6019783b849f5dc7d47b` | `https://cloud.cp.jku.at/public.php/dav/files/7ik4RrBKTS273gp/small0.ckpt` |

Meta released the `htdemucs` checkpoint with Demucs, whose repository is MIT-licensed. The
checkpoint has no separate licence file. It was trained on MUSDB18-HQ, which is
distributed for non-commercial research, plus additional internal training
data. SNDS treats the checkpoint as acceptable for private prototype
evaluation. Its terms should be re-evaluated before any commercial use or
redistribution of the weights. The checkpoint is never committed to this
repository.

The beat_this code and published weights are MIT-licensed. Its upstream README
notes that some training data had restrictive licences. Like `htdemucs`, the
checkpoint is downloaded and hash-verified during bootstrap, stored under
`%LOCALAPPDATA%`, and never committed.

## Known prototype-use caveats

- A JUCE licence decision is required before any binary release.
- Demucs is archived upstream; SNDS pins versions and isolates Demucs behind
  the versioned worker protocol instead of relying on upstream fixes.
- `htdemucs` weight terms are inferred from the Demucs repository and
  training-data description rather than an explicit weight licence. Both
  checkpoints were trained partly on data with restrictive licences.
- LGPL components (libsndfile, mpg123, LAME, lameenc, soxr) are used as unmodified,
  dynamically loaded packages installed by the user's own environment.
