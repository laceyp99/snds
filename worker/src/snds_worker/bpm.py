"""Global BPM estimation (in-house numpy/scipy estimator from spikes/python/bpm_compare.py).

Spectral-flux onset envelope -> global autocorrelation -> comb salience x log-normal prior at
120 BPM -> long-lag refinement. Returns BPM, a 0..1 confidence and the relative salience of the
half/double tempo so the app can offer half/double correction.

Signal choice: the separated ``drums`` stem is analysed first (the pulse without vocal/bass/pad
note changes that produce spurious onsets); the full mix is always analysed too and used when
the drums stem is near-silent or clearly less confident. Both candidates go into the manifest.
"""

from __future__ import annotations

import math
from typing import Any

import numpy as np
from scipy.signal import resample_poly

BPM_MIN = 40.0
BPM_MAX = 240.0
MIN_SECONDS = 5.0
ANALYSIS_SR = 22050
N_FFT = 2048
HOP = 256
FRAME_BATCH = 2048
DRUMS_MIN_RELATIVE_DB = -30.0  # drums stem quieter than this vs the mix -> use the mix
DRUMS_CONFIDENCE_MARGIN = 0.1
LOW_CONFIDENCE = 0.3
ESTIMATOR = {"name": "snds-acf-tempo", "version": 1}


def _band_matrix(n_fft: int, sr: int) -> np.ndarray:
    freqs = np.fft.rfftfreq(n_fft, 1 / sr)
    edges = np.geomspace(30, 11000, 25)
    m = np.zeros((freqs.size, 24), dtype=np.float32)
    for j, (lo, hi) in enumerate(zip(edges[:-1], edges[1:], strict=True)):
        m[(freqs >= lo) & (freqs < hi), j] = 1.0
    return m


def onset_envelope(x: np.ndarray, sr: int = ANALYSIS_SR) -> np.ndarray:
    """Band-wise log spectral flux. Frames are processed in batches to bound memory."""
    frames = 1 + (len(x) - N_FFT) // HOP
    window = np.hanning(N_FFT).astype(np.float32)
    bands_m = _band_matrix(N_FFT, sr)
    bands = np.empty((frames, bands_m.shape[1]), dtype=np.float32)
    offsets = np.arange(N_FFT)[None, :]
    for start in range(0, frames, FRAME_BATCH):
        stop = min(frames, start + FRAME_BATCH)
        idx = np.arange(start, stop)[:, None] * HOP + offsets
        spec = np.abs(np.fft.rfft(x[idx] * window, axis=1)).astype(np.float32)
        bands[start:stop] = spec @ bands_m
    logb = np.log1p(1000 * bands / (bands.max() + 1e-12))
    flux = np.maximum(0, np.diff(logb, axis=0)).sum(axis=1)
    fps = sr / HOP
    win = int(fps * 0.5) | 1
    flux = flux - np.convolve(flux, np.ones(win) / win, mode="same")
    return np.maximum(flux, 0).astype(np.float64)


def estimate_tempo(x: np.ndarray, sr: int) -> dict[str, Any]:
    """x: mono float signal. Returns {"status": "ok", "bpm", "confidence", "half", "double"} or
    {"status": "insufficient-audio" | "no-onsets"}."""
    x = np.asarray(x, dtype=np.float32)
    if len(x) < MIN_SECONDS * sr:
        return {"status": "insufficient-audio", "seconds": round(len(x) / sr, 3)}
    if sr != ANALYSIS_SR:
        g = math.gcd(sr, ANALYSIS_SR)
        x = resample_poly(x, ANALYSIS_SR // g, sr // g).astype(np.float32)
    if float(np.sqrt(np.mean(np.square(x, dtype=np.float64)))) < 1e-5:
        return {"status": "no-onsets"}
    onset = onset_envelope(x)
    std = onset.std()
    if not std > 1e-9:
        return {"status": "no-onsets"}
    onset /= std
    fps = ANALYSIS_SR / HOP

    m = len(onset)
    f = np.fft.rfft(onset - onset.mean(), n=2 * m)
    acf = np.fft.irfft(np.abs(f) ** 2)[:m]
    if not acf[0] > 0:
        return {"status": "no-onsets"}
    acf /= acf[0]

    def acf_at(lag: np.ndarray) -> np.ndarray:
        return np.interp(lag, np.arange(m), acf, right=0.0)

    grid = np.arange(BPM_MIN, BPM_MAX, 0.1)
    lags = 60.0 * fps / grid
    salience = np.maximum(sum(acf_at(k * lags) / k**0.5 for k in (1, 2, 3, 4)), 0)
    prior = np.exp(-0.5 * (np.log2(grid / 120.0) / 0.9) ** 2)
    score = salience * prior
    i = int(np.argmax(score))
    if not score[i] > 0:
        return {"status": "no-onsets"}
    # Refine within +-2 % using many beat multiples (long lags -> finer tempo resolution).
    fine = np.arange(grid[i] * 0.98, grid[i] * 1.02, 0.005)
    fine_lags = 60.0 * fps / fine
    kmax = max(1, int((m / 2) // fine_lags.max()))
    fine_sal = sum(acf_at(k * fine_lags) for k in range(1, min(kmax, 32) + 1))
    bpm = float(np.clip(fine[int(np.argmax(fine_sal))], BPM_MIN, BPM_MAX))

    def alternative(b: float) -> dict[str, Any] | None:
        if not BPM_MIN <= b <= BPM_MAX:
            return None
        j = int(np.argmin(np.abs(grid - b)))
        lo, hi = max(0, j - 5), min(len(grid), j + 6)
        rel = float(salience[lo:hi].max() / (salience[i] + 1e-12))
        return {"bpm": round(b, 2), "relative_salience": round(rel, 3)}

    # Confidence: how much the winner stands out from the best tempo that is neither within
    # 4 % of it nor an octave/third relative of it.
    related = np.zeros_like(grid, dtype=bool)
    for r in (1, 2, 0.5, 3, 1 / 3, 1.5, 2 / 3):
        related |= np.abs(grid / (bpm * r) - 1) < 0.04
    rival = float(score[~related].max()) if (~related).any() else 0.0
    confidence = float(np.clip(1 - rival / (score[i] + 1e-12), 0, 1))
    return {
        "status": "ok",
        "bpm": round(bpm, 2),
        "confidence": round(confidence, 3),
        "half": alternative(bpm / 2),
        "double": alternative(bpm * 2),
    }


def _rms_db(x: np.ndarray) -> float | None:
    ms = float(np.mean(np.square(x, dtype=np.float64))) if x.size else 0.0
    return 10 * math.log10(ms) if ms > 0 else None


def analyze_bpm(drums: np.ndarray | None, mix: np.ndarray, sr: int) -> dict[str, Any]:
    """Choose the global BPM from the drums stem (preferred) or the full mix (mono inputs)."""
    candidates: dict[str, Any] = {"mix": estimate_tempo(mix, sr)}
    drums_rel = None
    if drums is not None:
        d_db, m_db = _rms_db(drums), _rms_db(mix)
        if d_db is not None and m_db is not None:
            drums_rel = round(d_db - m_db, 2)
        if drums_rel is not None and drums_rel > DRUMS_MIN_RELATIVE_DB:
            candidates["drums"] = estimate_tempo(drums, sr)
        else:
            candidates["drums"] = {"status": "skipped-quiet"}
    drums_c = candidates.get("drums", {})
    mix_c = candidates["mix"]
    chosen_signal = None
    if drums_c.get("status") == "ok" and (
        mix_c.get("status") != "ok"
        or drums_c["confidence"] >= mix_c["confidence"] - DRUMS_CONFIDENCE_MARGIN
    ):
        chosen_signal = "drums"
    elif mix_c.get("status") == "ok":
        chosen_signal = "mix"
    result: dict[str, Any] = {
        "status": "ok" if chosen_signal else mix_c.get("status", "no-onsets"),
        "value": None,
        "confidence": None,
        "half": None,
        "double": None,
        "signal": chosen_signal,
        "range": [BPM_MIN, BPM_MAX],
        "estimator": dict(ESTIMATOR),
        "drums_relative_level_db": drums_rel,
        "candidates": candidates,
    }
    if chosen_signal:
        c = candidates[chosen_signal]
        result.update(
            value=c["bpm"], confidence=c["confidence"], half=c["half"], double=c["double"]
        )
    return result
