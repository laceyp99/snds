"""Spike: compare lightweight local BPM estimators (spike-bpm-library).

    # in-house estimator only (no extra deps):
    uv run --project worker --no-sync python spikes/python/bpm_compare.py
    # add librosa in a throwaway overlay env (does not touch worker/.venv or uv.lock):
    uv run --project worker --no-sync --with librosa==1.0.0 python spikes/python/bpm_compare.py

Test material is synthetic: drum patterns (four-on-the-floor, backbeat, half-time) and a full
pseudo-song, at 70/90/120/128/140/174 BPM, 30 s, 44.1 kHz stereo, with +-4 ms timing jitter
and velocity variation. Scores: Acc1 (within 4 % of nominal), Acc2 (within 4 % of nominal x
{1, 2, 1/2, 3, 1/3}). Runtime is measured warm (in-process) and cold (fresh interpreter:
import + first call), because the worker is a short-lived process per job.
"""

from __future__ import annotations

import json
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
from _common import synth_song
from scipy.signal import resample_poly

BPMS = [70.0, 90.0, 120.0, 128.0, 140.0, 174.0]
PATTERNS = ["four_on_floor", "backbeat", "half_time", "full_song"]
SR = 44100
SECONDS = 30.0


# --- synthetic drum patterns ------------------------------------------------------------


def drum_pattern(kind: str, bpm: float, seconds: float, sr: int, seed: int) -> np.ndarray:
    if kind == "full_song":
        return synth_song(seconds, sr=sr, bpm=bpm, seed=seed)
    rng = np.random.default_rng(seed)
    n = int(seconds * sr)
    out = np.zeros(n)

    def env(m: int, decay: float) -> np.ndarray:
        return np.exp(-np.arange(m) / (decay * sr))

    kt = np.arange(int(0.25 * sr)) / sr
    kick = np.sin(2 * np.pi * (50 + 90 * np.exp(-kt * 30)) * kt) * env(kt.size, 0.06)
    sn = rng.standard_normal(int(0.2 * sr)) * env(int(0.2 * sr), 0.04) * 0.6
    hat = np.diff(rng.standard_normal(int(0.04 * sr)) * env(int(0.04 * sr), 0.007), prepend=0)

    def hit(sig: np.ndarray, t: float, gain: float) -> None:
        i = int((t + rng.uniform(-0.004, 0.004)) * sr)
        if 0 <= i < n:
            m = min(sig.size, n - i)
            out[i : i + m] += gain * rng.uniform(0.8, 1.0) * sig[:m]

    beat = 60.0 / bpm
    for b in range(int(seconds / beat) + 1):
        t = b * beat
        pos = b % 4
        if kind == "four_on_floor":
            hit(kick, t, 1.0)
            if pos in (1, 3):
                hit(sn, t, 0.7)
            hit(hat, t + beat / 2, 0.4)
        elif kind == "backbeat":
            if pos in (0, 2):
                hit(kick, t, 1.0)
            if pos == 2:
                hit(kick, t + beat / 2, 0.7)
            if pos in (1, 3):
                hit(sn, t, 0.8)
            hit(hat, t, 0.3)
            hit(hat, t + beat / 2, 0.25)
        elif kind == "half_time":
            if pos == 0:
                hit(kick, t, 1.0)
            if pos == 2:
                hit(sn, t, 0.9)
            hit(hat, t, 0.3)
            hit(hat, t + beat / 2, 0.2)
    out *= 0.7 / np.abs(out).max()
    return np.stack([out, out], axis=1).astype(np.float32)


# --- in-house estimator (numpy/scipy only) ------------------------------------------------


def inhouse_tempo(y: np.ndarray, sr: int) -> dict:
    """Spectral-flux onset envelope -> global autocorrelation -> comb salience x log prior.

    Returns bpm, confidence in [0, 1] and half/double alternatives with relative salience.
    """
    x = y.mean(axis=1) if y.ndim == 2 else y
    target = 22050
    if sr != target:
        from math import gcd

        g = gcd(sr, target)
        x = resample_poly(x, target // g, sr // g)
    n_fft, hop = 2048, 256
    fps = target / hop
    frames = 1 + (len(x) - n_fft) // hop
    idx = np.arange(n_fft)[None, :] + hop * np.arange(frames)[:, None]
    spec = np.abs(np.fft.rfft(x[idx] * np.hanning(n_fft), axis=1))
    # 24 log-spaced bands between 30 Hz and 11 kHz, log compression, positive flux.
    freqs = np.fft.rfftfreq(n_fft, 1 / target)
    edges = np.geomspace(30, 11000, 25)
    bands = np.stack(
        [
            spec[:, (freqs >= lo) & (freqs < hi)].sum(axis=1)
            for lo, hi in zip(edges[:-1], edges[1:], strict=True)
        ],
        axis=1,
    )
    logb = np.log1p(1000 * bands / (bands.max() + 1e-12))
    flux = np.maximum(0, np.diff(logb, axis=0)).sum(axis=1)
    win = int(fps * 0.5) | 1
    flux = flux - np.convolve(flux, np.ones(win) / win, mode="same")
    onset = np.maximum(flux, 0)
    onset /= onset.std() + 1e-12

    m = len(onset)
    f = np.fft.rfft(onset - onset.mean(), n=2 * m)
    acf = np.fft.irfft(np.abs(f) ** 2)[:m]
    acf /= acf[0] + 1e-12

    def acf_at(lag: np.ndarray) -> np.ndarray:
        return np.interp(lag, np.arange(m), acf, right=0.0)

    bpm_grid = np.arange(40.0, 240.0, 0.1)
    lags = 60.0 * fps / bpm_grid
    salience = sum(acf_at(k * lags) / k**0.5 for k in (1, 2, 3, 4))
    salience = np.maximum(salience, 0)
    prior = np.exp(-0.5 * (np.log2(bpm_grid / 120.0) / 0.9) ** 2)
    score = salience * prior
    i = int(np.argmax(score))
    # Refine within +-2 % using many beat multiples (long lags => finer tempo resolution).
    fine = np.arange(bpm_grid[i] * 0.98, bpm_grid[i] * 1.02, 0.005)
    fine_lags = 60.0 * fps / fine
    kmax = max(1, int((m / 2) // fine_lags.max()))
    fine_sal = sum(acf_at(k * fine_lags) for k in range(1, min(kmax, 32) + 1))
    bpm = float(fine[int(np.argmax(fine_sal))])

    def rel(b: float) -> float:
        j = int(np.argmin(np.abs(bpm_grid - b)))
        lo, hi = max(0, j - 5), min(len(bpm_grid), j + 6)
        return round(float(salience[lo:hi].max() / (salience[i] + 1e-12)), 3)

    # Confidence: how much the winner stands out from the best tempo that is neither
    # within 4 % of it nor an octave/third relative of it.
    related = np.zeros_like(bpm_grid, dtype=bool)
    for r in (1, 2, 0.5, 3, 1 / 3, 1.5, 2 / 3):
        related |= np.abs(bpm_grid / (bpm * r) - 1) < 0.04
    rival = float(score[~related].max()) if (~related).any() else 0.0
    confidence = float(np.clip(1 - rival / (score[i] + 1e-12), 0, 1))
    return {
        "bpm": round(bpm, 2),
        "confidence": round(confidence, 3),
        "half_rel_salience": rel(bpm / 2),
        "double_rel_salience": rel(bpm * 2),
    }


# --- librosa adapter --------------------------------------------------------------------


def librosa_tempo(y: np.ndarray, sr: int) -> dict:
    import librosa

    mono = y.mean(axis=1)
    tempo = librosa.feature.tempo(y=mono, sr=sr)  # librosa 1.0 API
    return {"bpm": round(float(np.atleast_1d(tempo)[0]), 2)}


def librosa_beat_track(y: np.ndarray, sr: int) -> dict:
    import librosa

    tempo, beats = librosa.beat.beat_track(y=y.mean(axis=1), sr=sr)
    return {"bpm": round(float(np.atleast_1d(tempo)[0]), 2), "beats": int(len(beats))}


def available_estimators() -> dict:
    est = {"inhouse": inhouse_tempo}
    try:
        import librosa  # noqa: F401

        est["librosa_tempo"] = librosa_tempo
        est["librosa_beat_track"] = librosa_beat_track
    except ImportError:
        pass
    return est


def acc(est: float, truth: float, factors: tuple[float, ...]) -> bool:
    return any(abs(est / (truth * f) - 1) <= 0.04 for f in factors)


COLD_SNIPPET = r"""
import sys, time, json
t0 = time.perf_counter()
sys.path.insert(0, {spikes!r})
import numpy as np
import bpm_compare as b
y = b.drum_pattern("backbeat", 120.0, 30.0, 44100, 1)
t1 = time.perf_counter()
fn = b.available_estimators()[{name!r}]
r = fn(y, 44100)
t2 = time.perf_counter()
print(json.dumps({{"cold_import_and_call_s": round(t2 - t1, 3), "bpm": r["bpm"]}}))
"""


def cold_runtime(name: str) -> dict:
    spikes = str(Path(__file__).resolve().parent)
    code = COLD_SNIPPET.format(spikes=spikes, name=name)
    out = subprocess.run(
        [sys.executable, "-c", code], capture_output=True, text=True, timeout=600, check=False
    )
    lines = [ln for ln in out.stdout.splitlines() if ln.startswith("{")]
    return json.loads(lines[-1]) if lines else {"error": out.stderr[-300:]}


def main() -> int:
    ests = available_estimators()
    rows = []
    for pattern in PATTERNS:
        for bpm in BPMS:
            y = drum_pattern(pattern, bpm, SECONDS, SR, seed=int(bpm * 10))
            for name, fn in ests.items():
                t0 = time.perf_counter()
                r = fn(y, SR)
                dt = time.perf_counter() - t0
                row = {
                    "pattern": pattern,
                    "nominal_bpm": bpm,
                    "estimator": name,
                    **r,
                    "acc1": acc(r["bpm"], bpm, (1.0,)),
                    "acc2": acc(r["bpm"], bpm, (1.0, 2.0, 0.5, 3.0, 1 / 3)),
                    "seconds": round(dt, 3),
                }
                rows.append(row)
                print(json.dumps(row))
    summary = {}
    for name in ests:
        rs = [r for r in rows if r["estimator"] == name]
        summary[name] = {
            "acc1": f"{sum(r['acc1'] for r in rs)}/{len(rs)}",
            "acc2": f"{sum(r['acc2'] for r in rs)}/{len(rs)}",
            "warm_median_s": round(float(np.median([r["seconds"] for r in rs[1:]])), 3),
            **cold_runtime(name),
        }
    print(json.dumps({"summary": summary}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
