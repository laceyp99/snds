"""Compare BPM analyzers on deterministic synthetic patterns (and optional real audio).

Driver mode (default) renders clips into a temporary directory, runs every
candidate in its own subprocess (so import/model-load cold start is measured
cleanly and candidates may live in different environments), then prints
``key=value`` lines and Markdown tables.

    python compare.py [--candidates a,b] [--python name=PATH] [--real PATH]

Worker mode (internal): ``python compare.py --worker NAME --clips DIR --out FILE``.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

import numpy as np
import soundfile as sf

HERE = pathlib.Path(__file__).resolve().parent
SR = 44_100
CLIP_SECONDS = 30.0
TEMPOS = (70, 85, 96, 110, 120, 128, 140, 160, 174)
STYLES = (
    "four_on_floor",
    "rock_backbeat",
    "halftime_hiphop",
    "swung_shuffle",
    "breakbeat",
    "ambient_pulse",
)
TOLERANCE = 0.04  # MIREX "Accuracy 1" relative tolerance
DEFAULT_CANDIDATES = (
    "baseline",
    "librosa_tempo",
    "librosa_beat",
    "deeprhythm",
    "beat_this_small0",
    "beat_this_final0",
)
MODEL_DIR = pathlib.Path(os.environ.get("LOCALAPPDATA", tempfile.gettempdir())) / "snds-spikes" / "bpm-library-models"
DEEPRHYTHM_COMMIT = "b1f6116e0fe993322a0b3df2def54eb0c8d352f7"
MODEL_FILES = {
    "deeprhythm-0.7.pth": f"https://github.com/bleugreen/deeprhythm/raw/{DEEPRHYTHM_COMMIT}/deeprhythm-0.7.pth",
    "beat_this-final0.ckpt": "https://cloud.cp.jku.at/public.php/dav/files/7ik4RrBKTS273gp/final0.ckpt",
    "beat_this-small0.ckpt": "https://cloud.cp.jku.at/public.php/dav/files/7ik4RrBKTS273gp/small0.ckpt",
}

# --------------------------------------------------------------------------
# Synthetic material
# --------------------------------------------------------------------------


def _noise(rng: np.random.Generator, n: int) -> np.ndarray:
    return rng.standard_normal(n)


def _env(n: int, decay: float) -> np.ndarray:
    return np.exp(-np.arange(n) / (decay * SR))


def instrument(name: str, rng: np.random.Generator, pitch: float = 55.0) -> np.ndarray:
    if name == "kick":
        n = int(0.35 * SR)
        t = np.arange(n) / SR
        freq = 45 + 105 * np.exp(-t / 0.035)
        return np.sin(2 * np.pi * np.cumsum(freq) / SR) * _env(n, 0.13)
    if name == "pulse":  # soft, low thump for ambient material
        n = int(0.4 * SR)
        t = np.arange(n) / SR
        return 0.5 * np.sin(2 * np.pi * 60 * t) * np.minimum(1, t / 0.01) * _env(n, 0.18)
    if name == "snare":
        n = int(0.25 * SR)
        t = np.arange(n) / SR
        body = np.convolve(_noise(rng, n), [1, 0, -1], mode="same")
        return (0.6 * body + 0.5 * np.sin(2 * np.pi * 185 * t)) * _env(n, 0.07)
    if name == "clap":
        n = int(0.2 * SR)
        out = np.zeros(n)
        for offset in (0, int(0.011 * SR), int(0.023 * SR)):
            burst = np.convolve(_noise(rng, n - offset), [1, -1], mode="same") * _env(n - offset, 0.012)
            out[offset:] += burst
        return 0.5 * out
    if name in ("hat", "open_hat"):
        n = int((0.05 if name == "hat" else 0.3) * SR)
        sound = np.diff(_noise(rng, n + 1)) * _env(n, 0.012 if name == "hat" else 0.09)
        return 0.18 * sound
    if name == "rim":
        n = int(0.03 * SR)
        t = np.arange(n) / SR
        return 0.4 * np.sin(2 * np.pi * 1700 * t) * _env(n, 0.006)
    if name == "bass":
        n = int(0.22 * SR)
        t = np.arange(n) / SR
        wave = np.sin(2 * np.pi * pitch * t) + 0.35 * np.sin(4 * np.pi * pitch * t)
        return 0.45 * wave * np.minimum(1, t / 0.005) * np.minimum(1, (n - np.arange(n)) / (0.03 * SR))
    raise KeyError(name)


# steps per beat, then {instrument: [(step, velocity)]} per bar
PATTERNS: dict[str, tuple[int, dict[str, list[tuple[int, float]]]]] = {
    "four_on_floor": (4, {
        "kick": [(0, 1), (4, 1), (8, 1), (12, 1)],
        "clap": [(4, 0.8), (12, 0.8)],
        "hat": [(2, 0.9), (6, 0.9), (10, 0.9), (14, 0.9)],
        "bass": [(2, 0.8), (6, 0.8), (10, 0.8), (14, 0.8)],
    }),
    "rock_backbeat": (4, {
        "kick": [(0, 1), (8, 1), (10, 0.8)],
        "snare": [(4, 1), (12, 1)],
        "hat": [(s, 0.8 if s % 4 else 1.0) for s in range(0, 16, 2)],
        "bass": [(0, 0.9), (8, 0.9), (10, 0.7)],
    }),
    "halftime_hiphop": (4, {
        "kick": [(0, 1), (3, 0.7), (10, 0.9)],
        "snare": [(8, 1)],
        "hat": [(s, 0.7) for s in (0, 2, 4, 6, 8, 10, 12, 13, 14)],
        "bass": [(0, 0.9), (10, 0.8)],
    }),
    "swung_shuffle": (3, {
        "kick": [(0, 1), (6, 0.9)],
        "snare": [(3, 1), (9, 1)],
        "hat": [(s, 0.9 if s % 3 == 0 else 0.6) for s in (0, 2, 3, 5, 6, 8, 9, 11)],
        "bass": [(s, 0.7) for s in (0, 2, 3, 5, 6, 8, 9, 11)],
    }),
    "breakbeat": (4, {
        "kick": [(0, 1), (2, 0.6), (10, 0.9)],
        "snare": [(4, 1), (7, 0.35), (9, 0.4), (12, 1), (15, 0.35)],
        "hat": [(s, 0.7) for s in range(0, 14, 2)],
        "open_hat": [(14, 0.6)],
        "bass": [(0, 0.9), (7, 0.6), (10, 0.8)],
    }),
    "ambient_pulse": (4, {
        "pulse": [(0, 0.6), (4, 0.45), (8, 0.55), (12, 0.45)],
        "rim": [(8, 0.3)],
    }),
}


def render(style: str, bpm: float, seconds: float = CLIP_SECONDS, seed: int = 0) -> np.ndarray:
    rng = np.random.default_rng(seed)
    n = int(seconds * SR)
    out = np.zeros(n + SR)
    steps_per_beat, pattern = PATTERNS[style]
    beat = 60.0 / bpm
    bars = int(np.ceil(seconds / (4 * beat)))
    notes = 55.0 * 2 ** (np.array([0, 0, 3, 5, 7, 5, 3, 10]) / 12)
    for bar in range(bars):
        for name, hits in pattern.items():
            for step, velocity in hits:
                time_s = (bar * 4 + step / steps_per_beat) * beat + rng.normal(0, 0.004)
                if not 0 <= time_s < seconds:
                    continue
                pitch = notes[(bar * 2 + step) % len(notes)]
                sound = instrument(name, rng, pitch) * velocity * rng.uniform(0.9, 1.1)
                start = int(time_s * SR)
                out[start : start + len(sound)] += sound[: len(out) - start]
    out = out[:n]
    if style == "ambient_pulse":
        t = np.arange(n) / SR
        pad = sum(np.sin(2 * np.pi * f * t + p) for f, p in ((110, 0), (164.8, 1), (220, 2), (277.2, 3)))
        out += 0.12 * pad * (0.6 + 0.4 * np.sin(2 * np.pi * 0.05 * t))
        out += 0.01 * rng.standard_normal(n)
    return (0.8 * out / np.abs(out).max()).astype(np.float32)


def classify(estimate: float | None, truth: float) -> str:
    if estimate is None or not np.isfinite(estimate) or estimate <= 0:
        return "failed"
    for label, factor in (("exact", 1.0), ("half", 0.5), ("double", 2.0)):
        if abs(estimate - truth * factor) <= TOLERANCE * truth * factor:
            return label
    return "wrong"


# --------------------------------------------------------------------------
# Candidate adapters (imported lazily inside the worker subprocess)
# --------------------------------------------------------------------------


def ibi_tempo(beats: np.ndarray) -> tuple[float | None, float | None]:
    """Tempo from beat times plus a 0..1 regularity score.

    Beat trackers quantize beat times to their frame rate (beat_this: 20 ms),
    so the median interval alone is coarse. Averaging every interval within
    10 % of the median recovers sub-frame precision. Regularity is the share
    of intervals within 5 % of the median.
    """
    beats = np.asarray(beats, dtype=float)
    if len(beats) < 3:
        return None, None
    ibi = np.diff(beats)
    median = float(np.median(ibi))
    consistent = ibi[np.abs(ibi - median) <= 0.10 * median]
    regular = float(np.mean(np.abs(ibi - median) <= 0.05 * median))
    period = float(consistent.mean()) if len(consistent) else median  # bimodal intervals
    return 60.0 / period, regular


def fetch_model(name: str) -> pathlib.Path:
    import hashlib
    import urllib.request

    MODEL_DIR.mkdir(parents=True, exist_ok=True)
    path = MODEL_DIR / name
    if not path.exists():
        partial = path.with_suffix(path.suffix + ".part")
        urllib.request.urlretrieve(MODEL_FILES[name], partial)
        partial.replace(path)
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    print(f"model {name} bytes={path.stat().st_size} sha256={digest}", file=sys.stderr)
    return path


def load_candidate(name: str):
    """Return ``(estimate(y, sr) -> dict, info dict)`` for a candidate."""
    if name == "baseline":
        import bpm_baseline

        def estimate(y, sr):
            r = bpm_baseline.estimate_tempo(y, sr)
            return {"bpm": r.bpm, "confidence": r.confidence, "half_score": r.half_score,
                    "double_score": r.double_score, "score": r.score}

        return estimate, {"version": "in-house", "licence": "SNDS (MIT)"}

    if name in ("librosa_tempo", "librosa_beat"):
        import librosa

        if name == "librosa_tempo":
            def estimate(y, sr):
                return {"bpm": float(librosa.feature.tempo(y=y, sr=sr)[0]), "confidence": None}
        else:
            def estimate(y, sr):
                tempo, beats = librosa.beat.beat_track(y=y, sr=sr, units="time")
                bpm, regular = ibi_tempo(beats)
                return {"bpm": bpm, "confidence": regular, "tempo_field": float(np.atleast_1d(tempo)[0])}

        return estimate, {"version": librosa.__version__, "licence": "ISC"}

    if name == "deeprhythm":
        import importlib.metadata

        import librosa
        import torch
        from deeprhythm.model.predictor import DeepRhythmPredictor

        torch.set_grad_enabled(False)
        predictor = DeepRhythmPredictor.__new__(DeepRhythmPredictor)  # bypass its unpinned download
        predictor.model_path = str(fetch_model("deeprhythm-0.7.pth"))
        predictor.device = torch.device("cpu")
        predictor.model = predictor.load_model()
        predictor.specs = predictor.make_kernels()

        def estimate(y, sr):
            y22 = librosa.resample(y, orig_sr=sr, target_sr=22_050)
            if len(y22) < 8 * 22_050:
                return {"bpm": None, "confidence": None, "error": "needs >= 8 s"}
            bpm, confidence = predictor.predict_from_audio(y22, 22_050, include_confidence=True)
            return {"bpm": float(bpm), "confidence": float(confidence)}

        return estimate, {"version": importlib.metadata.version("deeprhythm"), "licence": "AGPL-3.0"}

    if name.startswith("beat_this_"):
        import importlib.metadata

        import torch
        from beat_this.inference import Audio2Beats

        torch.set_grad_enabled(False)
        checkpoint = fetch_model(f"beat_this-{name.removeprefix('beat_this_')}.ckpt")
        tracker = Audio2Beats(checkpoint_path=str(checkpoint), device="cpu", dbn=False)

        def estimate(y, sr):
            beats, _downbeats = tracker(y, sr)
            bpm, regular = ibi_tempo(beats)
            return {"bpm": bpm, "confidence": regular}

        return estimate, {"version": importlib.metadata.version("beat-this"), "licence": "MIT (code)"}

    if name == "aubio":
        import aubio

        def estimate(y, sr):
            hop = 512
            tracker = aubio.tempo("default", 1024, hop, sr)
            beats = []
            data = np.ascontiguousarray(y, dtype=np.float32)
            for start in range(0, len(data) - hop + 1, hop):
                if tracker(data[start : start + hop])[0]:
                    beats.append(tracker.get_last_s())
            bpm, _regular = ibi_tempo(np.array(beats))
            return {"bpm": bpm, "confidence": float(tracker.get_confidence()), "tracker_bpm": float(tracker.get_bpm())}

        return estimate, {"version": str(aubio.version), "licence": "GPL-3.0-or-later"}

    if name == "madmom":
        import madmom
        from madmom.audio.signal import Signal
        from madmom.features.beats import RNNBeatProcessor
        from madmom.features.tempo import TempoEstimationProcessor

        activations = RNNBeatProcessor()
        tempo = TempoEstimationProcessor(fps=100)

        def estimate(y, sr):
            result = tempo(activations(Signal(y, sample_rate=sr, num_channels=1)))
            return {"bpm": float(result[0][0]), "confidence": float(result[0][1])}

        return estimate, {"version": madmom.__version__, "licence": "BSD-3 code; CC BY-NC-SA 4.0 models"}

    raise KeyError(name)


def worker(name: str, clips_dir: pathlib.Path, out: pathlib.Path) -> None:
    sys.path.insert(0, str(HERE))
    started = time.perf_counter()
    estimate, info = load_candidate(name)
    load_seconds = time.perf_counter() - started
    manifest = json.loads((clips_dir / "manifest.json").read_text(encoding="utf-8"))
    rows = []
    first_call = None
    for clip in manifest:
        y, sr = sf.read(str(clips_dir / clip["file"]), dtype="float32", always_2d=True)
        y = np.ascontiguousarray(y.mean(axis=1))
        started = time.perf_counter()
        try:
            result = estimate(y, sr)
        except Exception as error:  # record and keep going
            result = {"bpm": None, "confidence": None, "error": f"{type(error).__name__}: {error}"[:200]}
        result["seconds"] = time.perf_counter() - started
        if first_call is None:
            first_call = result["seconds"]
        rows.append({**clip, **result})
    out.write_text(json.dumps({"candidate": name, "load_seconds": load_seconds, "first_call_seconds": first_call,
                               **info, "rows": rows}), encoding="utf-8")


# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------


def write_clips(directory: pathlib.Path, real: list[pathlib.Path]) -> list[dict]:
    manifest = []
    for style_index, style in enumerate(STYLES):
        for bpm in TEMPOS:
            name = f"{style}_{bpm}.wav"
            sf.write(str(directory / name), render(style, bpm, seed=style_index * 1000 + bpm), SR, subtype="FLOAT")
            manifest.append({"file": name, "kind": "synthetic", "style": style, "truth": bpm, "audio_seconds": CLIP_SECONDS})
    for index, path in enumerate(real):  # read-only; derived excerpts stay in the temp dir
        info = sf.info(str(path))
        label = f"real{index}"
        manifest.append({"file": str(path), "kind": "real", "style": f"{label}_full", "truth": None,
                         "audio_seconds": info.frames / info.samplerate})
        for excerpt_start in (0.0, 60.0):
            start = int(excerpt_start * info.samplerate)
            stop = start + int(12.0 * info.samplerate)
            if stop > info.frames:
                continue
            audio, rate = sf.read(str(path), start=start, stop=stop, dtype="float32", always_2d=True)
            name = f"{label}_excerpt_{int(excerpt_start)}s.wav"
            sf.write(str(directory / name), audio, rate, subtype="FLOAT")
            manifest.append({"file": name, "kind": "real", "style": f"{label}_12s_at_{int(excerpt_start)}s",
                             "truth": None, "audio_seconds": 12.0})
    (directory / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    return manifest


def report(results: list[dict]) -> None:
    summaries = []
    print()
    print("| candidate | version | licence | exact | half | double | wrong/failed | within 1 BPM | s per audio min | load s | first call s | confidence |")
    print("|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|")
    for result in results:
        rows = [r for r in result["rows"] if r["kind"] == "synthetic"]
        labels = [classify(r["bpm"], r["truth"]) for r in rows]
        within = sum(1 for r in rows if r["bpm"] and abs(r["bpm"] - r["truth"]) <= 1.0)
        steady = rows[1:]  # first call reported separately (JIT/warm-up)
        minutes = sum(r["audio_seconds"] for r in steady) / 60
        per_minute = sum(r["seconds"] for r in steady) / minutes if minutes else float("nan")
        has_conf = any(r.get("confidence") is not None for r in rows)
        bad = labels.count("wrong") + labels.count("failed")
        print(
            f"| {result['candidate']} | {result['version']} | {result['licence']} | {labels.count('exact')}/{len(rows)} "
            f"| {labels.count('half')} | {labels.count('double')} | {bad} | {within} | {per_minute:.3f} "
            f"| {result['load_seconds']:.2f} | {result['first_call_seconds']:.2f} | {'yes' if has_conf else 'no'} |"
        )
        summaries.append(
            f"candidate={result['candidate']} exact={labels.count('exact')} half={labels.count('half')} "
            f"double={labels.count('double')} wrong={labels.count('wrong')} failed={labels.count('failed')} "
            f"within_1bpm={within} seconds_per_audio_minute={per_minute:.4f} load_seconds={result['load_seconds']:.3f} "
            f"first_call_seconds={result['first_call_seconds']:.3f}"
        )

    print()
    for line in summaries:
        print(line)
    print()
    header = "| style | " + " | ".join(r["candidate"] for r in results) + " |"
    print(header)
    print("|---|" + "---|" * len(results))
    for style in STYLES:
        cells = []
        for result in results:
            rows = [r for r in result["rows"] if r["style"] == style]
            labels = [classify(r["bpm"], r["truth"]) for r in rows]
            cells.append(f"{labels.count('exact')}/{labels.count('half')}/{labels.count('double')}/"
                         f"{labels.count('wrong') + labels.count('failed')}")
        print(f"| {style} | " + " | ".join(cells) + " |")
    print("(cells: exact/half/double/wrong)")

    print()
    print("| clip | truth | " + " | ".join(r["candidate"] for r in results) + " |")
    print("|---|---:|" + "---:|" * len(results))
    for index, clip in enumerate(results[0]["rows"]):
        cells = []
        for result in results:
            row = result["rows"][index]
            if row["bpm"] is None:
                cells.append(f"fail ({row.get('error', '')[:30]})")
                continue
            confidence = "" if row.get("confidence") is None else f" c={row['confidence']:.2f}"
            label = "" if row["truth"] is None else f" {classify(row['bpm'], row['truth'])[0].upper()}"
            cells.append(f"{row['bpm']:.2f}{confidence}{label}")
        truth = "unverified" if clip["truth"] is None else clip["truth"]
        print(f"| {clip['style']} | {truth} | " + " | ".join(cells) + " |")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--worker")
    parser.add_argument("--clips", type=pathlib.Path)
    parser.add_argument("--out", type=pathlib.Path)
    parser.add_argument("--candidates", default=",".join(DEFAULT_CANDIDATES))
    parser.add_argument("--python", action="append", default=[], help="NAME=PATH interpreter override")
    parser.add_argument("--real", action="append", type=pathlib.Path, default=[], help="read-only real audio")
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()

    if args.worker:
        worker(args.worker, args.clips, args.out)
        return

    interpreters = dict(item.split("=", 1) for item in args.python)
    directory = pathlib.Path(tempfile.mkdtemp(prefix="snds-bpm-clips-"))
    try:
        started = time.perf_counter()
        manifest = write_clips(directory, args.real)
        print(f"started_at={time.strftime('%Y-%m-%dT%H:%M:%S%z')}")
        print(f"python={sys.version.split()[0]}")
        print(f"clips={len(manifest)} render_seconds={time.perf_counter() - started:.1f}")
        results = []
        for name in args.candidates.split(","):
            python = interpreters.get(name, sys.executable)
            out = directory / f"result-{name}.json"
            started = time.perf_counter()
            done = subprocess.run(
                [python, "-X", "utf8", str(HERE / "compare.py"), "--worker", name, "--clips", str(directory), "--out", str(out)],
                capture_output=True, text=True, encoding="utf-8", errors="replace",
            )
            print(f"candidate={name} exit={done.returncode} wall_seconds={time.perf_counter() - started:.1f}")
            if done.returncode != 0:
                print(f"candidate={name} error={done.stderr.strip().splitlines()[-1:]}")
                continue
            for line in done.stderr.splitlines():
                if line.startswith("model "):
                    print(f"candidate={name} {line}")
            results.append(json.loads(out.read_text(encoding="utf-8")))
        print(f"finished_at={time.strftime('%Y-%m-%dT%H:%M:%S%z')}")
        report(results)
    finally:
        if args.keep:
            print(f"clips_dir={directory}")
        else:
            shutil.rmtree(directory, ignore_errors=True)


if __name__ == "__main__":
    main()
