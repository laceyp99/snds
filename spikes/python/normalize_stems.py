"""Spike: normalized stems are exactly aligned to the source (verify-spike-audio-alignment).

    uv run --project worker --no-sync python spikes/python/normalize_stems.py [--device cpu|cuda]
    uv run --project worker --no-sync python spikes/python/normalize_stems.py --resampler-only

Prototype pipeline per case (synthetic sources only, written as real WAV files first):

    soundfile decode (float32, always 2-D)
      -> mono upmix to 2 ch (duplicate)       (model wants stereo)
      -> resample source_sr -> 44100 (polyphase, zero-phase)
      -> htdemucs apply_model(shifts=0, split=True, overlap=0.25)
      -> resample each stem 44100 -> source_sr
      -> trim / zero-pad to EXACTLY the source frame count
      -> write 4 x stereo float32 WAV at source_sr (no clipping)
      -> re-open the WAVs and validate: frames, rate, channels, finite, alignment

Alignment metrics (sum of stems vs. source upmixed to stereo):
  * integer lag of the full-file cross-correlation peak (+ parabolic sub-sample estimate)
  * lag in 1 s windows at start / middle / end (detects drift / rate errors)
  * residual energy of (sum - source) relative to the source, full band and <= 20 kHz
  * the same residual for a resampler-only round trip (no model) to separate the causes
  * peak level per stem and of the stem sum (dBFS), and count of samples beyond +-1.0
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from fractions import Fraction
from pathlib import Path

import numpy as np
import soundfile as sf
from _common import EXPECTED_SOURCES, Timer, load_htdemucs, output_dir, synth_song
from scipy.signal import butter, fftconvolve, resample_poly, sosfiltfilt

MODEL_SR = 44100

# (name, sample rate, channels, frames, WAV subtype, drive)  drive > 1 = loud, clipped master
CASES = [
    ("44k1_stereo_pcm16", 44100, 2, 44100 * 10 + 12345, "PCM_16", 1.0),
    ("48k_stereo_pcm24", 48000, 2, 48000 * 10 + 777, "PCM_24", 1.0),
    ("96k_stereo_float", 96000, 2, 96000 * 10 + 1, "FLOAT", 1.0),
    ("192k_stereo_pcm24", 192000, 2, 192000 * 10 + 3, "PCM_24", 1.0),
    ("22k05_stereo_pcm16", 22050, 2, 22050 * 10 + 5, "PCM_16", 1.0),
    ("44k1_mono_pcm16", 44100, 1, 44100 * 10 + 3, "PCM_16", 1.0),
    ("48k_mono_pcm24", 48000, 1, 48000 * 10 + 11, "PCM_24", 1.0),
    ("44k1_stereo_hot_master", 44100, 2, 44100 * 10, "PCM_16", 4.0),
]


# --- resamplers -----------------------------------------------------------------------


def resample_scipy(x: np.ndarray, sr_in: int, sr_out: int) -> np.ndarray:
    """x: (frames, ch) float. Zero-phase polyphase FIR (Kaiser), output ceil(n*up/down)."""
    if sr_in == sr_out:
        return x
    r = Fraction(sr_out, sr_in)
    return resample_poly(x, r.numerator, r.denominator, axis=0).astype(np.float32)


def resample_julius(x: np.ndarray, sr_in: int, sr_out: int) -> np.ndarray:
    import julius
    import torch

    if sr_in == sr_out:
        return x
    t = torch.from_numpy(np.ascontiguousarray(x.T, dtype=np.float32))
    y = julius.resample_frac(t, sr_in, sr_out, full=True)
    return y.numpy().T.copy()


RESAMPLERS = {"scipy": resample_scipy, "julius": resample_julius}


def fit_length(x: np.ndarray, frames: int) -> np.ndarray:
    if x.shape[0] >= frames:
        return x[:frames]
    return np.concatenate([x, np.zeros((frames - x.shape[0], x.shape[1]), x.dtype)], axis=0)


# --- metrics ---------------------------------------------------------------------------


def xcorr_lag(a: np.ndarray, b: np.ndarray, max_lag: int = 4096) -> tuple[int, float]:
    """Lag (in samples) that best aligns b to a; + parabolic sub-sample refinement."""
    a = a - a.mean()
    b = b - b.mean()
    c = fftconvolve(a, b[::-1], mode="full")
    zero = len(b) - 1
    lo, hi = max(0, zero - max_lag), min(len(c), zero + max_lag + 1)
    k = lo + int(np.argmax(c[lo:hi]))
    frac = 0.0
    if 0 < k < len(c) - 1:
        y0, y1, y2 = c[k - 1], c[k], c[k + 1]
        den = y0 - 2 * y1 + y2
        frac = 0.5 * (y0 - y2) / den if den != 0 else 0.0
    return k - zero, float(k - zero + frac)


def windowed_lags(a: np.ndarray, b: np.ndarray, sr: int) -> list[int]:
    w = sr  # 1 s windows
    starts = [0, (len(a) - w) // 2, len(a) - w]
    return [xcorr_lag(a[s : s + w], b[s : s + w], max_lag=512)[0] for s in starts]


def db(x: float) -> float:
    return round(10 * math.log10(max(x, 1e-30)), 2)


def residual_db(ref: np.ndarray, est: np.ndarray, sr: int, band_hz: float | None = None) -> float:
    err = est - ref
    if band_hz is not None and band_hz < sr / 2:
        sos = butter(8, band_hz, fs=sr, output="sos")
        err = sosfiltfilt(sos, err, axis=0)
        ref = sosfiltfilt(sos, ref, axis=0)
    return db(float(np.sum(err**2)) / float(np.sum(ref**2)))


def peak_dbfs(x: np.ndarray) -> float:
    return round(20 * math.log10(max(float(np.abs(x).max()), 1e-12)), 2)


# --- pipeline --------------------------------------------------------------------------


def make_source(path: Path, sr: int, ch: int, frames: int, subtype: str, drive: float) -> None:
    x = synth_song(frames / sr + 0.01, sr=sr, channels=ch, seed=sr + ch)[:frames]
    if drive != 1.0:  # loud master: soft clip then normalise to -0.1 dBFS
        x = np.tanh(drive * x)
        x = (x / np.abs(x).max() * 10 ** (-0.1 / 20)).astype(np.float32)
    sf.write(path, x, sr, subtype=subtype)


def separate(model, x44: np.ndarray, device: str) -> np.ndarray:
    """x44: (frames, 2) at 44.1 kHz -> (4, frames, 2)."""
    import torch
    from demucs.apply import apply_model

    mix = torch.from_numpy(np.ascontiguousarray(x44.T))[None]
    with torch.inference_mode():
        out = apply_model(model, mix, shifts=0, split=True, overlap=0.25, device=device)
    return out[0].cpu().numpy().transpose(0, 2, 1)


def run_case(model, case, resampler: str, device: str, out_root: Path) -> dict:
    name, sr, ch, frames, subtype, drive = case
    rs = RESAMPLERS[resampler]
    case_dir = out_root / name
    case_dir.mkdir(parents=True, exist_ok=True)
    src_path = case_dir / "source.wav"
    make_source(src_path, sr, ch, frames, subtype, drive)

    info = sf.info(str(src_path))
    x, sr_read = sf.read(str(src_path), dtype="float32", always_2d=True)
    src_stereo = np.repeat(x, 2, axis=1) if x.shape[1] == 1 else x
    row: dict[str, object] = {
        "case": name,
        "source_sr": sr,
        "source_channels": ch,
        "source_subtype": info.subtype,
        "frames_expected": frames,
        "frames_header": info.frames,
        "frames_decoded": x.shape[0],
    }

    with Timer() as t_rs_in:
        x44 = rs(src_stereo, sr, MODEL_SR)
    with Timer() as t_sep:
        stems44 = separate(model, x44, device)
    with Timer() as t_rs_out:
        stems = np.stack([fit_length(rs(s, MODEL_SR, sr), frames) for s in stems44])
    row["model_frames"] = x44.shape[0]
    row["resample_seconds"] = round(t_rs_in.seconds + t_rs_out.seconds, 3)
    row["separate_seconds"] = round(t_sep.seconds, 2)

    for i, stem in enumerate(EXPECTED_SOURCES):
        sf.write(case_dir / f"{stem}.wav", stems[i], sr, subtype="FLOAT")

    # Re-open what was written: this is what the C++ app will stream.
    back = []
    ok_contract = True
    for stem in EXPECTED_SOURCES:
        si = sf.info(str(case_dir / f"{stem}.wav"))
        y, _ = sf.read(str(case_dir / f"{stem}.wav"), dtype="float32", always_2d=True)
        ok_contract &= si.samplerate == sr and si.channels == 2 and si.frames == frames
        ok_contract &= y.shape == (frames, 2) and bool(np.isfinite(y).all())
        back.append(y)
    back = np.stack(back)
    total = back.sum(axis=0)

    a = src_stereo.mean(axis=1).astype(np.float64)
    b = total.mean(axis=1).astype(np.float64)
    lag, lag_frac = xcorr_lag(a, b)
    rt = fit_length(rs(x44, MODEL_SR, sr), frames)  # resampler-only round trip
    rt_lag, rt_lag_frac = xcorr_lag(a, rt.mean(axis=1).astype(np.float64))
    band = min(20000.0, 0.45 * sr)
    row.update(
        stems_contract_ok=bool(ok_contract),
        frames_equal=all(s.shape[0] == frames for s in back),
        lag_samples=lag,
        lag_subsample=round(lag_frac, 4),
        window_lags_start_mid_end=windowed_lags(a, b, sr),
        residual_db_full=residual_db(src_stereo, total, sr),
        residual_db_le_20k=residual_db(src_stereo, total, sr, band),
        resampler_only_lag=rt_lag,
        resampler_only_lag_subsample=round(rt_lag_frac, 4),
        resampler_only_residual_db_le_20k=residual_db(src_stereo, rt, sr, band),
        source_peak_dbfs=peak_dbfs(src_stereo),
        stem_peak_dbfs={s: peak_dbfs(back[i]) for i, s in enumerate(EXPECTED_SOURCES)},
        stem_sum_peak_dbfs=peak_dbfs(total),
        samples_over_0dbfs=int(sum(int((np.abs(s) > 1.0).sum()) for s in back)),
    )
    return row


def resampler_only(out_root: Path) -> list[dict]:
    """Compare scipy vs julius round trips (source_sr -> 44.1k -> source_sr), no model."""
    rows = []
    for name, sr, ch, frames, _subtype, _drive in CASES:
        if sr == MODEL_SR:
            continue
        x = synth_song(frames / sr + 0.01, sr=sr, channels=2, seed=sr + ch)[:frames]
        for rname, rs in RESAMPLERS.items():
            with Timer() as t:
                y44 = rs(x, sr, MODEL_SR)
                y = rs(y44, MODEL_SR, sr)
            n_back = y.shape[0]
            y = fit_length(y, frames)
            a = x.mean(axis=1).astype(np.float64)
            lag, lag_frac = xcorr_lag(a, y.mean(axis=1).astype(np.float64))
            band = min(20000.0, 0.45 * sr)
            rows.append(
                {
                    "case": name,
                    "resampler": rname,
                    "frames_44k1": y44.shape[0],
                    "frames_back_before_trim": n_back,
                    "lag": lag,
                    "lag_subsample": round(lag_frac, 4),
                    "residual_db_le_20k": residual_db(x, y, sr, band),
                    "residual_db_le_16k": residual_db(x, y, sr, min(16000.0, 0.36 * sr)),
                    "seconds": round(t.seconds, 3),
                }
            )
    return rows


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", choices=["cpu", "cuda"], default="cpu")
    ap.add_argument("--resampler", choices=list(RESAMPLERS), default="scipy")
    ap.add_argument("--resampler-only", action="store_true")
    args = ap.parse_args()
    out_root = output_dir("normalize")

    if args.resampler_only:
        rows = resampler_only(out_root)
        for r in rows:
            print(json.dumps(r))
        (out_root / "resampler_compare.json").write_text(json.dumps(rows, indent=2))
        return 0

    model = load_htdemucs(args.device)
    rows = []
    for case in CASES:
        row = run_case(model, case, args.resampler, args.device, out_root)
        row["resampler"] = args.resampler
        row["device"] = args.device
        rows.append(row)
        print(json.dumps(row))
        sys.stdout.flush()
    (out_root / f"results_{args.resampler}_{args.device}.json").write_text(
        json.dumps(rows, indent=2)
    )
    ok = all(r["stems_contract_ok"] and r["frames_equal"] and r["lag_samples"] == 0 for r in rows)
    print(json.dumps({"all_cases_exact_frames_and_zero_lag": ok}))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
