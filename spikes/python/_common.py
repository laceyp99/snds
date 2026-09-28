"""Shared helpers for the Phase 0 Python spikes (not part of the worker package).

- Pinned htdemucs checkpoint constants and a hash-verified local loader.
- Deterministic synthetic test audio (no user music is ever read).
- Output directory handling (``spikes/output/``, git-ignored).
"""

from __future__ import annotations

import hashlib
import os
import time
from fractions import Fraction
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[2]
OUTPUT_DIR = REPO_ROOT / "spikes" / "output"

# --- Pinned model artifact -------------------------------------------------------------
# demucs `htdemucs` == bag of exactly one model with signature 955717e8
# (demucs/remote/htdemucs.yaml: "models: ['955717e8']", files.txt root hybrid_transformer/).
HTDEMUCS_NAME = "htdemucs"
HTDEMUCS_SIG = "955717e8"
HTDEMUCS_FILE = "955717e8-8726e21a.th"
HTDEMUCS_URL = "https://dl.fbaipublicfiles.com/demucs/hybrid_transformer/" + HTDEMUCS_FILE
HTDEMUCS_SIZE = 84_141_911
HTDEMUCS_SHA256 = "8726e21a993978c7ba086d3872e7608d7d5bfca646ca4aca459ffda844faa8b4"
HTDEMUCS_BAG_YAML = "models: ['955717e8']\n"
EXPECTED_SOURCES = ["drums", "bass", "other", "vocals"]


def models_dir() -> Path:
    """App-controlled model directory: %LOCALAPPDATA%\\snds\\models\\htdemucs."""
    base = os.environ.get("LOCALAPPDATA")
    if not base:
        raise RuntimeError("LOCALAPPDATA is not set")
    return Path(base) / "snds" / "models" / HTDEMUCS_NAME


def sha256_file(path: Path, chunk: int = 1 << 20) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while block := f.read(chunk):
            h.update(block)
    return h.hexdigest()


def verify_checkpoint(path: Path) -> None:
    size = path.stat().st_size
    if size != HTDEMUCS_SIZE:
        raise RuntimeError(f"{path} has size {size}, expected {HTDEMUCS_SIZE}")
    digest = sha256_file(path)
    if digest != HTDEMUCS_SHA256:
        raise RuntimeError(f"{path} sha256 {digest} != pinned {HTDEMUCS_SHA256}")


def _weights_only_allowlist() -> list:
    """Exact pickle globals the pinned checkpoint needs (from
    ``torch.serialization.get_unsafe_globals_in_checkpoint``), nothing more."""
    from demucs.htdemucs import HTDemucs

    return [
        HTDemucs,
        Fraction,
        (np._core.multiarray.scalar, "numpy.core.multiarray.scalar"),
        np.dtype,
        np.dtypes.Float64DType,
    ]


def load_htdemucs(device: str = "cpu", *, verify: bool = True):
    """Load the pinned model from the local models dir, never from torch.hub / HF cache.

    1. Verify size + full SHA-256 of the pinned file.
    2. ``torch.load(weights_only=True)`` with a 5-entry allowlist (no arbitrary pickle code;
       demucs' own loader uses ``weights_only=False``).
    3. Build the model with ``demucs.states.load_model(package_dict)``.

    Returns a bare ``HTDemucs``. The ``htdemucs`` bag holds exactly this one model with
    weights [1, 1, 1, 1], so applying the bare model is numerically the same thing.
    """
    import torch
    from demucs.states import load_model

    ckpt = models_dir() / HTDEMUCS_FILE
    if verify:
        verify_checkpoint(ckpt)
    with torch.serialization.safe_globals(_weights_only_allowlist()):
        package = torch.load(ckpt, map_location="cpu", weights_only=True)
    model = load_model(package)
    if list(model.sources) != EXPECTED_SOURCES:
        raise RuntimeError(f"unexpected sources {model.sources}")
    if model.samplerate != 44100 or model.audio_channels != 2:
        raise RuntimeError("unexpected model samplerate/channels")
    model.to(device)
    model.eval()
    return model


def load_htdemucs_via_demucs_repo():
    """Upstream path for comparison: ``demucs.pretrained.get_model('htdemucs', repo=dir)``.

    Returns the ``BagOfModels`` (1 model). demucs checks the 8-hex filename checksum only.
    """
    from demucs.pretrained import get_model

    return get_model(HTDEMUCS_NAME, repo=models_dir())


def output_dir(sub: str) -> Path:
    d = OUTPUT_DIR / sub
    d.mkdir(parents=True, exist_ok=True)
    return d


# --- Synthetic audio -------------------------------------------------------------------


def synth_song(
    seconds: float,
    sr: int = 44100,
    bpm: float = 120.0,
    channels: int = 2,
    seed: int = 1234,
) -> np.ndarray:
    """Deterministic pseudo-song: kick/snare/hat transients, bass line, formant 'vocal', pad.

    Returns float32 array shaped (frames, channels) with peak ~ -3 dBFS.
    """
    rng = np.random.default_rng(seed)
    n = round(seconds * sr)
    t = np.arange(n) / sr
    beat = 60.0 / bpm
    out = np.zeros((n, 2), dtype=np.float64)

    def env(length_s: float, decay: float) -> np.ndarray:
        m = round(length_s * sr)
        return np.exp(-np.arange(m) / (decay * sr))

    kick_len = round(0.25 * sr)
    kt = np.arange(kick_len) / sr
    kick = np.sin(2 * np.pi * (50 + 90 * np.exp(-kt * 30)) * kt) * env(0.25, 0.06)
    snare = rng.standard_normal(round(0.2 * sr)) * env(0.2, 0.04) * 0.6
    snare += np.sin(2 * np.pi * 190 * np.arange(snare.size) / sr) * env(0.2, 0.03) * 0.4
    hat = rng.standard_normal(round(0.05 * sr)) * env(0.05, 0.008) * 0.25
    hat = np.diff(hat, prepend=0.0)  # crude high-pass

    def place(sig: np.ndarray, start_s: float, pan: float) -> None:
        i0 = round(start_s * sr)
        if i0 >= n:
            return
        m = min(sig.size, n - i0)
        out[i0 : i0 + m, 0] += sig[:m] * (1 - pan) ** 0.5
        out[i0 : i0 + m, 1] += sig[:m] * (1 + pan) ** 0.5 / 1.0

    n_beats = int(seconds / beat) + 1
    for b in range(n_beats):
        tb = b * beat
        place(kick * 0.9, tb, 0.0)
        if b % 2 == 1:
            place(snare, tb, 0.1)
        place(hat, tb + beat / 2, -0.4)
        place(hat * 0.6, tb, 0.4)

    # Bass: root notes every bar (4 beats), sine + a little 2nd harmonic.
    roots = [55.0, 55.0 * 2 ** (3 / 12), 55.0 * 2 ** (7 / 12), 55.0 * 2 ** (5 / 12)]
    bar = 4 * beat
    f_bass = np.array([roots[int(x // bar) % 4] for x in t])
    phase = 2 * np.pi * np.cumsum(f_bass) / sr
    bass = 0.35 * (np.sin(phase) + 0.25 * np.sin(2 * phase))
    out[:, 0] += bass
    out[:, 1] += bass

    # "Vocal": glottal-ish pulse train through formant resonators, with vibrato & phrases.
    from scipy.signal import lfilter

    f0 = 220.0 * 2 ** (np.round(3 * np.sin(2 * np.pi * t / (2 * bar))) / 12)
    f0 = f0 * (1 + 0.01 * np.sin(2 * np.pi * 5.5 * t))
    vphase = np.cumsum(f0) / sr
    pulses = np.diff(np.floor(vphase), prepend=0.0)
    voice = np.zeros(n)
    for fc, bw, g in ((700, 110, 1.0), (1220, 120, 0.6), (2600, 160, 0.3)):
        r = np.exp(-np.pi * bw / sr)
        a = [1, -2 * r * np.cos(2 * np.pi * fc / sr), r * r]
        voice += g * lfilter([1 - r], a, pulses)
    phrase = (np.sin(2 * np.pi * t / (2 * bar)) > -0.3).astype(np.float64)
    voice *= phrase * 0.5 / max(1e-9, np.abs(voice).max())
    out[:, 0] += voice
    out[:, 1] += voice

    # Pad: detuned saw-ish chord, wide stereo.
    pad = np.zeros((n, 2))
    for k, f in enumerate((220.0, 277.18, 329.63)):
        for ch, det in ((0, 0.997), (1, 1.003)):
            ph = 2 * np.pi * f * det * t + k
            pad[:, ch] += sum(np.sin(h * ph) / h for h in range(1, 6))
    out += 0.05 * pad

    out *= 10 ** (-3 / 20) / np.abs(out).max()
    if channels == 1:
        return out.mean(axis=1, keepdims=True).astype(np.float32)
    return out.astype(np.float32)


class Timer:
    def __enter__(self) -> Timer:
        self.t0 = time.perf_counter()
        return self

    def __exit__(self, *exc: object) -> None:
        self.seconds = time.perf_counter() - self.t0


__all__ = [
    "EXPECTED_SOURCES",
    "HTDEMUCS_BAG_YAML",
    "HTDEMUCS_FILE",
    "HTDEMUCS_SHA256",
    "HTDEMUCS_SIZE",
    "HTDEMUCS_URL",
    "Timer",
    "load_htdemucs",
    "load_htdemucs_via_demucs_repo",
    "models_dir",
    "output_dir",
    "sha256_file",
    "synth_song",
    "verify_checkpoint",
]
