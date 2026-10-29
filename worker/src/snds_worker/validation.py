"""Output validation before cache finalization (the agreed normalization tolerance).

Hard failures:
  - every stem exists, is a WAV / 32-bit float / 2 channels / source sample rate;
  - frames == decoded source frames exactly (tolerance 0), all samples finite;
  - integer lag between stem sum and source == 0 (aggregate and in every analysed window);
  - in-band residual of (stem sum - source) <= -10 dB.
Warnings: in-band residual > -20 dB, |sub-sample lag| > 0.05 samples.
Stems are never clipped, limited or normalized; peaks are recorded.
"""

from __future__ import annotations

import hashlib
import math
import os
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import numpy as np
import soundfile as sf
from scipy.signal import fftconvolve

from snds_worker.audio import peak_dbfs
from snds_worker.model import SOURCES

RESIDUAL_FAIL_DB = -10.0
RESIDUAL_WARN_DB = -20.0
SUBSAMPLE_WARN = 0.05
MAX_LAG = 2048
LAG_WINDOWS = 5
LAG_WINDOW_SECONDS = 2.0
SILENCE_RMS = 1e-4  # -80 dBFS: windows quieter than this carry no alignment information
STFT_SIZE = 16384
STFT_BATCH = 64
TOLERANCE = {
    "frames": "exact",
    "lag_samples": 0,
    "subsample_warn": SUBSAMPLE_WARN,
    "residual_fail_db": RESIDUAL_FAIL_DB,
    "residual_warn_db": RESIDUAL_WARN_DB,
}


@dataclass
class ValidationReport:
    failures: list[dict[str, Any]] = field(default_factory=list)
    warnings: list[dict[str, Any]] = field(default_factory=list)
    metrics: dict[str, Any] = field(default_factory=dict)
    stems: dict[str, dict[str, Any]] = field(default_factory=dict)

    @property
    def ok(self) -> bool:
        return not self.failures

    @property
    def status(self) -> str:
        if self.failures:
            return "failed"
        return "passed-with-warnings" if self.warnings else "passed"

    def fail(self, code: str, message: str, **detail: Any) -> None:
        self.failures.append({"code": code, "message": message, **detail})

    def warn(self, code: str, message: str, **detail: Any) -> None:
        self.warnings.append({"code": code, "message": message, **detail})

    def to_json(self) -> dict[str, Any]:
        return {
            "status": self.status,
            "tolerance": dict(TOLERANCE),
            "failures": self.failures,
            "warnings": self.warnings,
            "metrics": self.metrics,
        }


def _sha256_and_sync(path: Path) -> str:
    """Hash the file and flush it to stable storage (FlushFileBuffers needs write access)."""
    h = hashlib.sha256()
    with path.open("r+b") as f:
        while block := f.read(1 << 20):
            h.update(block)
        os.fsync(f.fileno())
    return h.hexdigest()


def xcorr(a: np.ndarray, b: np.ndarray, max_lag: int) -> np.ndarray:
    """Cross-correlation of b against a for lags -max_lag..+max_lag."""
    a = a - a.mean()
    b = b - b.mean()
    c = fftconvolve(a, b[::-1], mode="full")
    zero = len(b) - 1
    return c[zero - max_lag : zero + max_lag + 1]


def _peak_lag(c: np.ndarray, max_lag: int) -> tuple[int, float]:
    """(integer, sub-sample) lag; positive means the estimate is late vs. the reference."""
    k = int(np.argmax(c))
    frac = 0.0
    if 0 < k < len(c) - 1:
        y0, y1, y2 = c[k - 1], c[k], c[k + 1]
        den = y0 - 2 * y1 + y2
        frac = 0.5 * (y0 - y2) / den if den != 0 else 0.0
    return max_lag - k, float(max_lag - k - frac)


def alignment(source_mono: np.ndarray, sum_mono: np.ndarray, sr: int) -> dict[str, Any]:
    """Integer and sub-sample lag in LAG_WINDOWS windows spread over the file plus their
    aggregate (catches constant offsets and drift). Silent windows are skipped."""
    n = len(source_mono)
    w = min(n, int(LAG_WINDOW_SECONDS * sr))
    max_lag = max(1, min(MAX_LAG, w - 1))
    starts = sorted({int(s) for s in np.linspace(0, n - w, LAG_WINDOWS)})
    window_lags: list[dict[str, Any]] = []
    total = np.zeros(2 * max_lag + 1)
    for s in starts:
        a = source_mono[s : s + w].astype(np.float64)
        b = sum_mono[s : s + w].astype(np.float64)
        if float(np.sqrt(np.mean(a * a))) < SILENCE_RMS:
            continue
        c = xcorr(a, b, max_lag)
        total += c / (np.max(np.abs(c)) + 1e-30)
        lag, _ = _peak_lag(c, max_lag)
        window_lags.append({"start_frame": s, "lag": lag})
    if not window_lags:
        return {"applicable": False, "reason": "source is silent", "windows": []}
    lag, sub = _peak_lag(total, max_lag)
    return {
        "applicable": True,
        "lag_samples": lag,
        "lag_subsample": round(sub, 4),
        "max_lag_searched": max_lag,
        "window_seconds": round(w / sr, 3),
        "windows": window_lags,
    }


def band_residual_db(
    ref: np.ndarray, est: np.ndarray, sr: int, band_hz: float
) -> tuple[float | None, float | None]:
    """(in-band, full-band) energy of (est - ref) relative to ref, in dB.

    In-band energy uses Hann-windowed 50 %-overlap STFT frames and only bins <= band_hz, so the
    >22.05 kHz content that htdemucs cannot reproduce for high-rate sources is excluded."""
    n = ref.shape[0]
    err_full = ref_full = 0.0
    for s in range(0, n, 1 << 20):  # chunked: no full-length float64 temporaries
        r = ref[s : s + (1 << 20)].astype(np.float64)
        e = est[s : s + (1 << 20)].astype(np.float64) - r
        err_full += float(np.sum(e * e))
        ref_full += float(np.sum(r * r))
    full = 10 * math.log10(max(err_full, 1e-30) / ref_full) if ref_full > 1e-12 else None
    size = STFT_SIZE
    while size > 256 and size > n:
        size //= 2
    hop = size // 2
    win = np.hanning(size + 1)[:-1].astype(np.float32)[:, None]
    mask = np.fft.rfftfreq(size, 1 / sr) <= band_hz
    starts = list(range(0, max(1, n - size + 1), hop))
    if starts[-1] + size < n:
        starts.append(n - size if n >= size else 0)
    e_ref = e_err = 0.0
    for i in range(0, len(starts), STFT_BATCH):
        batch = starts[i : i + STFT_BATCH]
        r = np.stack([_frame(ref, s, size) for s in batch])
        d = np.stack([_frame(est, s, size) for s in batch]) - r
        rs = np.fft.rfft(r * win, axis=1)[:, mask]
        ds = np.fft.rfft(d * win, axis=1)[:, mask]
        e_ref += float(np.sum(np.abs(rs) ** 2))
        e_err += float(np.sum(np.abs(ds) ** 2))
    inband = 10 * math.log10(max(e_err, 1e-30) / e_ref) if e_ref > 1e-12 else None
    return (
        None if inband is None else round(inband, 2),
        None if full is None else round(full, 2),
    )


def _frame(x: np.ndarray, start: int, size: int) -> np.ndarray:
    f = x[start : start + size]
    if f.shape[0] < size:
        f = np.pad(f, ((0, size - f.shape[0]), (0, 0)))
    return f


def validate_stems(
    stems_dir: Path,
    *,
    source: np.ndarray,
    samplerate: int,
    check: Callable[[], None] | None = None,
    progress: Callable[[float], None] | None = None,
) -> ValidationReport:
    """Re-open the written stems exactly as the app will and check the contract.

    ``source`` is the decoded source upmixed to stereo, shape (frames, 2)."""
    frames = int(source.shape[0])
    report = ValidationReport()
    total = np.zeros((frames, 2), dtype=np.float32)
    readable = 0
    over_0dbfs = 0
    for i, name in enumerate(SOURCES):
        if check is not None:
            check()
        path = stems_dir / f"{name}.wav"
        entry: dict[str, Any] = {"path": f"stems/{name}.wav"}
        report.stems[name] = entry
        if not path.is_file():
            report.fail("missing-stem", f"{name}.wav is missing", stem=name)
            continue
        try:
            info = sf.info(str(path))
            data, _ = sf.read(str(path), dtype="float32", always_2d=True)
        except (sf.LibsndfileError, RuntimeError, OSError) as exc:
            report.fail("unreadable-stem", f"{name}.wav cannot be read: {exc}", stem=name)
            continue
        entry.update(
            samplerate=info.samplerate,
            channels=info.channels,
            frames=int(data.shape[0]),
            format=info.format,
            subtype=info.subtype,
        )
        ok = True
        if info.format != "WAV" or info.subtype != "FLOAT":
            report.fail(
                "wrong-format",
                f"{name}.wav is {info.format}/{info.subtype}, expected WAV/FLOAT",
                stem=name,
            )
            ok = False
        if info.channels != 2:
            report.fail(
                "wrong-channel-count", f"{name}.wav has {info.channels} channels", stem=name
            )
            ok = False
        if info.samplerate != samplerate:
            report.fail(
                "wrong-sample-rate",
                f"{name}.wav is {info.samplerate} Hz, source is {samplerate} Hz",
                stem=name,
            )
            ok = False
        if data.shape[0] != frames or info.frames != frames:
            report.fail(
                "wrong-frame-count",
                f"{name}.wav has {data.shape[0]} frames, source has {frames}",
                stem=name,
                frames=int(data.shape[0]),
                expected=frames,
            )
            ok = False
        finite = np.isfinite(data)
        if not finite.all():
            report.fail(
                "non-finite-samples",
                f"{name}.wav contains {int((~finite).sum())} NaN/Inf samples",
                stem=name,
            )
            ok = False
        else:
            entry["peak_dbfs"] = peak_dbfs(data)
            over_0dbfs += int(np.count_nonzero(np.abs(data) > 1.0))
        if ok:
            total += data
            readable += 1
        del data, finite
        entry["bytes"] = path.stat().st_size
        entry["sha256"] = _sha256_and_sync(path)
        if progress is not None:
            progress((i + 1) / (len(SOURCES) + 1))

    report.metrics["stems_checked"] = readable
    if readable != len(SOURCES):
        return report

    report.metrics["samples_over_0dbfs"] = over_0dbfs
    report.metrics["stem_sum_peak_dbfs"] = peak_dbfs(total)
    report.metrics["source_peak_dbfs"] = peak_dbfs(source)
    if check is not None:
        check()
    align = alignment(source.mean(axis=1), total.mean(axis=1), samplerate)
    report.metrics["alignment"] = align
    if align["applicable"]:
        bad = [w for w in align["windows"] if w["lag"] != 0]
        if align["lag_samples"] != 0 or bad:
            report.fail(
                "misaligned",
                f"stem sum is offset from the source by {align['lag_samples']} samples",
                lag_samples=align["lag_samples"],
                windows=bad,
            )
        elif abs(align["lag_subsample"]) > SUBSAMPLE_WARN:
            report.warn(
                "subsample-lag",
                f"sub-sample lag {align['lag_subsample']} exceeds {SUBSAMPLE_WARN}",
                lag_subsample=align["lag_subsample"],
            )
    if check is not None:
        check()
    band = min(20000.0, 0.45 * samplerate)
    inband, full = band_residual_db(source, total, samplerate, band)
    report.metrics["residual_db_inband"] = inband
    report.metrics["residual_db_fullband"] = full
    report.metrics["residual_band_hz"] = band
    if inband is not None:
        if inband > RESIDUAL_FAIL_DB:
            report.fail(
                "residual-too-high",
                f"stem sum differs from the source by {inband} dB (limit {RESIDUAL_FAIL_DB} dB)",
                residual_db=inband,
            )
        elif inband > RESIDUAL_WARN_DB:
            report.warn(
                "residual-high",
                f"stem sum differs from the source by {inband} dB "
                f"(warn above {RESIDUAL_WARN_DB} dB)",
                residual_db=inband,
            )
    if progress is not None:
        progress(1.0)
    return report
