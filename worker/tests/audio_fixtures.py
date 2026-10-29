"""Deterministic synthetic audio for tests (no user music). Ported from spikes/python/_common.py."""

from __future__ import annotations

from pathlib import Path

import numpy as np
import soundfile as sf
from scipy.signal import lfilter


def synth_song(
    seconds: float, sr: int = 44100, bpm: float = 120.0, channels: int = 2, seed: int = 1234
) -> np.ndarray:
    """Kick/snare/hat transients, bass line, formant 'vocal' and pad; (frames, ch) float32 at
    about -3 dBFS peak."""
    rng = np.random.default_rng(seed)
    n = round(seconds * sr)
    t = np.arange(n) / sr
    beat = 60.0 / bpm
    out = np.zeros((n, 2), dtype=np.float64)

    def env(length_s: float, decay: float) -> np.ndarray:
        return np.exp(-np.arange(round(length_s * sr)) / (decay * sr))

    kt = np.arange(round(0.25 * sr)) / sr
    kick = np.sin(2 * np.pi * (50 + 90 * np.exp(-kt * 30)) * kt) * env(0.25, 0.06)
    snare = rng.standard_normal(round(0.2 * sr)) * env(0.2, 0.04) * 0.6
    snare += np.sin(2 * np.pi * 190 * np.arange(snare.size) / sr) * env(0.2, 0.03) * 0.4
    hat = np.diff(rng.standard_normal(round(0.05 * sr)) * env(0.05, 0.008) * 0.25, prepend=0.0)

    def place(sig: np.ndarray, start_s: float, pan: float) -> None:
        i0 = round(start_s * sr)
        if i0 >= n:
            return
        m = min(sig.size, n - i0)
        out[i0 : i0 + m, 0] += sig[:m] * (1 - pan) ** 0.5
        out[i0 : i0 + m, 1] += sig[:m] * (1 + pan) ** 0.5

    for b in range(int(seconds / beat) + 1):
        tb = b * beat
        place(kick * 0.9, tb, 0.0)
        if b % 2 == 1:
            place(snare, tb, 0.1)
        place(hat, tb + beat / 2, -0.4)
        place(hat * 0.6, tb, 0.4)

    roots = [55.0, 55.0 * 2 ** (3 / 12), 55.0 * 2 ** (7 / 12), 55.0 * 2 ** (5 / 12)]
    bar = 4 * beat
    f_bass = np.array([roots[int(x // bar) % 4] for x in t])
    phase = 2 * np.pi * np.cumsum(f_bass) / sr
    bass = 0.35 * (np.sin(phase) + 0.25 * np.sin(2 * phase))
    out += bass[:, None]

    f0 = 220.0 * 2 ** (np.round(3 * np.sin(2 * np.pi * t / (2 * bar))) / 12)
    f0 = f0 * (1 + 0.01 * np.sin(2 * np.pi * 5.5 * t))
    pulses = np.diff(np.floor(np.cumsum(f0) / sr), prepend=0.0)
    voice = np.zeros(n)
    for fc, bw, g in ((700, 110, 1.0), (1220, 120, 0.6), (2600, 160, 0.3)):
        r = np.exp(-np.pi * bw / sr)
        voice += g * lfilter([1 - r], [1, -2 * r * np.cos(2 * np.pi * fc / sr), r * r], pulses)
    phrase = (np.sin(2 * np.pi * t / (2 * bar)) > -0.3).astype(np.float64)
    voice *= phrase * 0.5 / max(1e-9, np.abs(voice).max())
    out += voice[:, None]

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


def drum_loop(bpm: float, seconds: float, sr: int = 44100, seed: int = 1) -> np.ndarray:
    """Backbeat drum pattern with +-4 ms jitter, (frames, 2) float32."""
    rng = np.random.default_rng(seed)
    n = int(seconds * sr)
    out = np.zeros(n)
    kt = np.arange(int(0.25 * sr)) / sr
    kick = np.sin(2 * np.pi * (50 + 90 * np.exp(-kt * 30)) * kt) * np.exp(-kt / 0.06)
    sn = rng.standard_normal(int(0.2 * sr)) * np.exp(-np.arange(int(0.2 * sr)) / (0.04 * sr)) * 0.6
    hat = np.diff(
        rng.standard_normal(int(0.04 * sr)) * np.exp(-np.arange(int(0.04 * sr)) / (0.007 * sr)),
        prepend=0,
    )

    def hit(sig: np.ndarray, t0: float, gain: float) -> None:
        i = int((t0 + rng.uniform(-0.004, 0.004)) * sr)
        if 0 <= i < n:
            m = min(sig.size, n - i)
            out[i : i + m] += gain * rng.uniform(0.8, 1.0) * sig[:m]

    beat = 60.0 / bpm
    for b in range(int(seconds / beat) + 1):
        t0 = b * beat
        pos = b % 4
        if pos in (0, 2):
            hit(kick, t0, 1.0)
        if pos in (1, 3):
            hit(sn, t0, 0.8)
        hit(hat, t0, 0.3)
        hit(hat, t0 + beat / 2, 0.25)
    out *= 0.7 / np.abs(out).max()
    return np.stack([out, out], axis=1).astype(np.float32)


def write_wav(path: Path, x: np.ndarray, sr: int, subtype: str = "PCM_16") -> Path:
    sf.write(str(path), x, sr, format="WAV", subtype=subtype)
    return path


def write_mp3(path: Path, x: np.ndarray, sr: int) -> Path:
    sf.write(str(path), x, sr, format="MP3", subtype="MPEG_LAYER_III")
    return path
