"""Self-contained NumPy global tempo estimator (onset autocorrelation baseline).

Pipeline:
1. Band-wise log-magnitude spectral flux at 100 frames/s (onset envelope).
2. Local-mean removal and half-wave rectification.
3. Normalized autocorrelation of the envelope.
4. Harmonic comb score per tempo (lags at 1..4 beat periods) weighted by a
   log-normal tempo prior centred on 120 BPM (one octave deviation), the same
   prior shape librosa uses by default.
5. Fine refinement within +/-2 % using up to 8 beat periods of lag.

Returns the tempo, a 0..1 confidence (normalized autocorrelation at the beat
period), and half/double alternatives with their comb scores so the UI can
offer metrical corrections. Only NumPy is required.
"""

from __future__ import annotations

import dataclasses

import numpy as np

FPS = 100.0
MIN_BPM = 40.0
MAX_BPM = 240.0
PRIOR_CENTER_BPM = 120.0
PRIOR_OCTAVES = 1.0


@dataclasses.dataclass
class TempoEstimate:
    bpm: float
    confidence: float
    half_bpm: float
    half_score: float
    double_bpm: float
    double_score: float
    score: float


def onset_envelope(y: np.ndarray, sr: int) -> np.ndarray:
    """Band-wise log spectral flux sampled at ``FPS`` frames per second."""
    y = np.asarray(y, dtype=np.float64)
    if y.ndim == 2:
        y = y.mean(axis=1)
    hop = sr / FPS
    n_fft = 1 << int(np.ceil(np.log2(0.046 * sr)))
    count = max(1, int((len(y) - n_fft) / hop) + 1)
    if len(y) < n_fft:
        y = np.pad(y, (0, n_fft - len(y)))
    starts = (np.arange(count) * hop).astype(np.int64)
    window = np.hanning(n_fft)
    edges = np.geomspace(30.0, min(16_000.0, sr / 2 * 0.95), 41)
    bins = np.clip(np.round(edges / sr * n_fft).astype(int), 1, n_fft // 2)
    bands = np.empty((count, len(bins) - 1))
    for begin in range(0, count, 512):  # bounded memory for long tracks
        idx = starts[begin : begin + 512, None] + np.arange(n_fft)[None, :]
        spectrum = np.abs(np.fft.rfft(y[idx] * window, axis=1))
        cumulative = np.cumsum(spectrum, axis=1)
        totals = cumulative[:, bins[1:] - 1] - cumulative[:, bins[:-1] - 1]
        bands[begin : begin + 512] = totals / np.maximum(1, bins[1:] - bins[:-1])
    log_bands = np.log1p(1000.0 * bands)
    flux = np.maximum(0.0, np.diff(log_bands, axis=0, prepend=log_bands[:1])).sum(axis=1)
    kernel = np.ones(int(FPS * 0.5)) / int(FPS * 0.5)
    local_mean = np.convolve(flux, kernel, mode="same")
    return np.maximum(0.0, flux - local_mean)


def autocorrelation(envelope: np.ndarray, max_lag: int) -> np.ndarray:
    centred = envelope - envelope.mean()
    nfft = 1 << int(np.ceil(np.log2(2 * len(centred))))
    spectrum = np.fft.rfft(centred, nfft)
    ac = np.fft.irfft(spectrum * np.conj(spectrum), nfft)[: max_lag + 1]
    ac /= np.arange(len(centred), len(centred) - max_lag - 1, -1)  # unbiased
    return ac / ac[0] if ac[0] > 0 else ac * 0


def _interp(ac: np.ndarray, lags: np.ndarray) -> np.ndarray:
    return np.interp(lags, np.arange(len(ac)), ac, right=0.0)


def comb_score(ac: np.ndarray, bpm: np.ndarray, harmonics: int = 4) -> np.ndarray:
    bpm = np.asarray(bpm, dtype=np.float64)
    period = 60.0 * FPS / bpm
    weights = 1.0 / np.arange(1, harmonics + 1)
    score = np.zeros_like(bpm)
    for k, weight in enumerate(weights, start=1):
        score += weight * np.maximum(0.0, _interp(ac, k * period))
    return score / weights.sum()


def prior(bpm: np.ndarray) -> np.ndarray:
    return np.exp(-0.5 * (np.log2(bpm / PRIOR_CENTER_BPM) / PRIOR_OCTAVES) ** 2)


def estimate_tempo(y: np.ndarray, sr: int) -> TempoEstimate:
    envelope = onset_envelope(y, sr)
    max_lag = min(len(envelope) - 1, int(8 * 60.0 * FPS / MIN_BPM))
    ac = autocorrelation(envelope, max_lag)
    grid = np.arange(MIN_BPM, MAX_BPM + 0.25, 0.25)
    raw = comb_score(ac, grid)
    weighted = raw * prior(grid)
    coarse = float(grid[int(np.argmax(weighted))])

    fine = np.linspace(coarse * 0.98, coarse * 1.02, 801)
    usable = max(1, min(8, int(max_lag / (60.0 * FPS / coarse))))
    refined = fine[int(np.argmax(comb_score(ac, fine, harmonics=usable)))]
    bpm = float(refined)

    def score_at(value: float) -> float:
        if not MIN_BPM / 2 <= value <= MAX_BPM * 2:
            return 0.0
        return float(comb_score(ac, np.array([value]))[0])

    period = 60.0 * FPS / bpm
    confidence = float(np.clip(_interp(ac, np.array([period]))[0], 0.0, 1.0))
    return TempoEstimate(
        bpm=bpm,
        confidence=confidence,
        half_bpm=bpm / 2,
        half_score=score_at(bpm / 2),
        double_bpm=bpm * 2,
        double_score=score_at(bpm * 2),
        score=score_at(bpm),
    )
