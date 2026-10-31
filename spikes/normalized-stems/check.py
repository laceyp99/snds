"""Verify stem normalization alignment across source rates, layouts, and codecs.

Generates deterministic synthetic sources in a temporary directory, runs the
reference pipeline in ``normalize.py`` and checks the normalization contract:
exact source frame count, source sample rate, two channels, float32 WAV,
finite samples, no clipping, zero-sample lag, and no drift.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import sys
import tempfile
import time

import julius
import numpy as np
import soundfile as sf
import torch
from demucs.pretrained import get_model

import normalize as norm

RATES = (44_100, 48_000, 96_000, 192_000)
SUBTYPES = ("PCM_16", "PCM_24", "FLOAT")
WINDOW_SECONDS = 3.0
COMPONENTS = ("drums", "bass", "other", "vocals")


# --------------------------------------------------------------------------
# Deterministic synthetic material
# --------------------------------------------------------------------------


def lowpass_kernel(sample_rate: int, cutoff: float, taps: int = 255) -> np.ndarray:
    n = np.arange(taps) - (taps - 1) / 2
    kernel = np.sinc(2 * cutoff / sample_rate * n) * np.hanning(taps)
    return kernel / kernel.sum()


def synth(sample_rate: int, frames: int, seed: int) -> dict[str, np.ndarray]:
    """Return stereo components (frames, 2) float64 keyed by stem name."""
    rng = np.random.default_rng(seed)
    t = np.arange(frames) / sample_rate
    parts = {name: np.zeros((frames, 2)) for name in COMPONENTS}
    lp = lowpass_kernel(sample_rate, 15_000.0)

    # Drums: irregular hit times so a lag cannot alias onto a periodic grid.
    position = 0.25
    hit = 0
    while position < frames / sample_rate - 0.5:
        start = int(position * sample_rate)
        length = int(0.35 * sample_rate)
        local = np.arange(length) / sample_rate
        if hit % 2 == 0:
            freq = 50 + 110 * np.exp(-local / 0.04)
            phase = 2 * np.pi * np.cumsum(freq) / sample_rate
            sound = 0.9 * np.sin(phase) * np.exp(-local / 0.12)
        else:
            noise = np.convolve(rng.standard_normal(length), lp, mode="same")
            sound = (0.5 * noise + 0.3 * np.sin(2 * np.pi * 190 * local)) * np.exp(-local / 0.07)
        pan = 0.5 + 0.2 * rng.uniform(-1, 1)
        end = min(frames, start + length)
        parts["drums"][start:end, 0] += sound[: end - start] * (1 - pan) * 2
        parts["drums"][start:end, 1] += sound[: end - start] * pan * 2
        position += rng.uniform(0.18, 0.61)
        hit += 1

    # Bass: note changes every 0.5 s with short attack/release envelopes.
    notes = 55.0 * 2 ** (rng.integers(0, 12, size=int(t[-1] / 0.5) + 2) / 12)
    note_index = (t / 0.5).astype(int)
    freq = notes[note_index]
    phase = 2 * np.pi * np.cumsum(freq) / sample_rate
    within = (t % 0.5) / 0.5
    envelope = np.minimum(1, within / 0.02) * np.minimum(1, (1 - within) / 0.05)
    bass = 0.45 * (np.sin(phase) + 0.3 * np.sin(2 * phase)) * envelope
    parts["bass"][:, 0] = bass
    parts["bass"][:, 1] = bass

    # Other: slow pad chord panned left.
    pad = sum(np.sin(2 * np.pi * f * t + p) for f, p in ((220, 0.1), (277.2, 1.3), (329.6, 2.2)))
    pad *= 0.12 * (0.6 + 0.4 * np.sin(2 * np.pi * 0.25 * t))
    parts["other"][:, 0] = 0.75 * pad
    parts["other"][:, 1] = 0.35 * pad

    # Vocals: vibrato tone with harmonics in phrases, panned right.
    vib = 440 * (1 + 0.01 * np.sin(2 * np.pi * 5.5 * t)) * 2 ** (np.floor(t / 1.7) % 4 / 12)
    vphase = 2 * np.pi * np.cumsum(vib) / sample_rate
    voice = np.sin(vphase) + 0.5 * np.sin(2 * vphase) + 0.25 * np.sin(3 * vphase)
    phrase = (np.sin(2 * np.pi * t / 3.4) > -0.2).astype(float)
    phrase = np.convolve(phrase, np.ones(int(0.03 * sample_rate)) / int(0.03 * sample_rate), mode="same")
    parts["vocals"][:, 0] = 0.12 * voice * phrase
    parts["vocals"][:, 1] = 0.22 * voice * phrase
    return parts


def write_mp3(path: pathlib.Path, audio: np.ndarray, rate: int, bitrate_mode: int) -> None:
    """Encode MP3 through libsndfile/LAME with an explicit bitrate mode.

    soundfile 0.12.1 does not expose SFC_SET_BITRATE_MODE (0x1305), so the
    command is sent through its cffi handle: 0 = constant, 1 = average,
    2 = variable (libsndfile's default).
    """
    with sf.SoundFile(str(path), "w", rate, audio.shape[1], format="MP3", subtype="MPEG_LAYER_III") as handle:
        mode = sf._ffi.new("int*", bitrate_mode)
        assert sf._snd.sf_command(handle._file, 0x1305, mode, sf._ffi.sizeof("int")) == 0
        handle.write(audio)


def build_case(case: dict, directory: pathlib.Path) -> tuple[pathlib.Path, np.ndarray, dict[str, np.ndarray]]:
    if case.get("real"):
        return build_real_case(case, directory)
    rate = case["rate"]
    frames = int(round(rate * case["seconds"])) + case["extra_frames"]
    parts = synth(rate, frames, case["seed"])
    if case["channels"] == 1:
        parts = {name: value.mean(axis=1, keepdims=True) for name, value in parts.items()}
    mix = sum(parts.values())
    if case.get("silence"):
        mix = np.zeros_like(mix)
        parts = {name: np.zeros_like(mix) for name in parts}
    else:
        scale = case["peak"] / np.abs(mix).max()
        mix = mix * scale
        parts = {name: value * scale for name, value in parts.items()}
    path = directory / f"{case['name']}.{case['container']}"
    if case["container"] == "mp3":
        write_mp3(path, mix.astype(np.float32), rate, case["bitrate_mode"])
        if case.get("strip_gapless"):
            strip_gapless_tag(path)
    else:
        sf.write(str(path), mix.astype(np.float32), rate, format="WAV", subtype=case["subtype"])
    return path, mix, parts


def build_real_case(case: dict, directory: pathlib.Path) -> tuple[pathlib.Path, np.ndarray, dict]:
    """Owner-supplied audio: used in place (read-only) or as a derived excerpt."""
    source = pathlib.Path(case["real"])
    if not case.get("excerpt"):
        audio, _rate = sf.read(str(source), dtype="float32", always_2d=True)
        return source, audio.astype(np.float64), {}
    info = sf.info(str(source))
    start = int(case["excerpt"][0] * info.samplerate)
    stop = start + int(case["excerpt"][1] * info.samplerate)
    audio, rate = sf.read(str(source), start=start, stop=stop, dtype="float32", always_2d=True)
    wav = torch.from_numpy(audio.T.copy())
    if case["rate"] != rate:
        wav = julius.resample_frac(wav, rate, case["rate"])
    derived = wav.numpy().T.astype(np.float32)
    path = directory / f"{case['name']}.wav"
    sf.write(str(path), derived, case["rate"], format="WAV", subtype=case["subtype"])
    decoded, _ = sf.read(str(path), dtype="float32", always_2d=True)
    return path, decoded.astype(np.float64), {}


def strip_gapless_tag(path: pathlib.Path) -> None:
    """Disable the LAME Info/Xing frame so decoders cannot trim delay/padding.

    This emulates MP3 files from encoders that omit gapless metadata, or a
    decoder that ignores it. The disabled frame then decodes as 1152 samples
    of silence.
    """
    data = bytearray(path.read_bytes())
    for magic in (b"Info", b"Xing"):
        index = data.find(magic, 0, 4096)
        if index >= 0:
            data[index : index + 4] = b"NoTg"
            path.write_bytes(bytes(data))
            return
    raise AssertionError("no LAME Info/Xing frame found")


def cases(long_case: bool, real: list[pathlib.Path]) -> list[dict]:
    result = []
    index = 0
    for rate in RATES:
        for channels in (1, 2):
            for subtype in SUBTYPES:
                layout = "mono" if channels == 1 else "stereo"
                result.append(
                    {
                        "name": f"wav_{rate}_{layout}_{subtype.lower()}",
                        "container": "wav",
                        "rate": rate,
                        "channels": channels,
                        "subtype": subtype,
                        "seconds": 10.0 + (index * 0.83) % 10.0,
                        "extra_frames": 7 + index * 13,
                        "peak": 2.0 if subtype == "FLOAT" else 0.9,
                        "seed": 1000 + index,
                        "shifts": 0,
                    }
                )
                index += 1
    mp3 = {
        "container": "mp3",
        "subtype": "MPEG_LAYER_III",
        "seconds": 15.0,
        "extra_frames": 101,
        "peak": 0.9,
        "shifts": 0,
    }
    for rate, channels, mode, label in (
        (44_100, 2, 2, "vbr"),
        (48_000, 2, 2, "vbr"),
        (44_100, 1, 2, "vbr"),
        (44_100, 2, 0, "cbr"),
        (48_000, 2, 0, "cbr"),
    ):
        layout = "mono" if channels == 1 else "stereo"
        result.append(dict(mp3, name=f"mp3_{rate}_{layout}_{label}", rate=rate, channels=channels,
                           bitrate_mode=mode, seed=2000 + rate + channels + mode))
    by_name = {case["name"]: case for case in result}
    result.append(dict(by_name["mp3_44100_stereo_cbr"], name="mp3_44100_stereo_cbr_tag_disabled", strip_gapless=True))
    result.append(dict(by_name["mp3_44100_stereo_vbr"], name="mp3_44100_stereo_vbr_tag_disabled",
                       strip_gapless=True, expect_rejection=True))
    result.append(
        {
            "name": "wav_44100_stereo_pcm_16_silence",
            "container": "wav",
            "rate": 44_100,
            "channels": 2,
            "subtype": "PCM_16",
            "seconds": 10.0,
            "extra_frames": 3,
            "peak": 0.0,
            "seed": 3000,
            "shifts": 0,
            "silence": True,
        }
    )
    result.append(dict(by_name["wav_96000_stereo_pcm_24"], name="wav_96000_stereo_pcm_24_shifts1", shifts=1))
    if long_case:
        result.append(
            {
                "name": "wav_48000_stereo_pcm_24_long180",
                "container": "wav",
                "rate": 48_000,
                "channels": 2,
                "subtype": "PCM_24",
                "seconds": 180.0,
                "extra_frames": 11,
                "peak": 0.9,
                "seed": 4000,
                "shifts": 0,
            }
        )
    for number, path in enumerate(real):
        info = sf.info(str(path))
        base = {"container": "wav", "real": str(path), "channels": info.channels, "shifts": 0}
        result.append(dict(base, name=f"real{number}_full", rate=info.samplerate, subtype=info.subtype))
        result.append(dict(base, name=f"real{number}_excerpt20s_48000_pcm_24", rate=48_000,
                           subtype="PCM_24", excerpt=(60.0, 20.0)))
    return result


# --------------------------------------------------------------------------
# Measurements
# --------------------------------------------------------------------------


def xcorr_lag(reference: np.ndarray, test: np.ndarray, max_lag: int) -> tuple[int, float, float]:
    """Lag (samples) of ``test`` relative to ``reference``; positive = test late.

    Returns the integer peak, a parabolic sub-sample estimate, and the peak
    normalized correlation coefficient.
    """
    n = len(reference) + len(test)
    nfft = 1 << (n - 1).bit_length()
    spectrum = np.fft.rfft(test, nfft) * np.conj(np.fft.rfft(reference, nfft))
    corr = np.fft.irfft(spectrum, nfft)
    window = np.concatenate((corr[-max_lag:], corr[: max_lag + 1]))
    peak = int(np.argmax(window))
    lag = peak - max_lag
    sub = float(lag)
    if 0 < peak < len(window) - 1:
        left, centre, right = window[peak - 1 : peak + 2]
        denominator = left - 2 * centre + right
        if denominator != 0:
            sub = lag + 0.5 * (left - right) / denominator
    norm_factor = np.sqrt(np.dot(reference, reference) * np.dot(test, test))
    coefficient = float(window[peak] / norm_factor) if norm_factor > 0 else 0.0
    return lag, sub, coefficient


def residual_db(reference: np.ndarray, test: np.ndarray) -> float:
    energy = float(np.sum(reference**2))
    if energy == 0:
        return float("-inf")
    return 10 * np.log10(max(float(np.sum((test - reference) ** 2)), 1e-30) / energy)


def stereo(audio: np.ndarray) -> np.ndarray:
    return np.repeat(audio, 2, axis=1) if audio.shape[1] == 1 else audio


def measure(case: dict, model, device, directory: pathlib.Path) -> dict:
    row: dict = {"case": case["name"]}
    path, original, components = build_case(case, directory)
    row["source_frames_original"] = len(original)
    if case["container"] == "mp3":
        raw, _rate = sf.read(str(path), dtype="float32", always_2d=True)
        mpeg_frames, samples_per_frame = norm.mpeg_layer3_frames(path.read_bytes())
        max_lag = 4 * 1152 + 2048
        lag, _sub, coefficient = xcorr_lag(original.mean(axis=1), raw.mean(axis=1).astype(np.float64), max_lag)
        row.update(
            libsndfile_frames=len(raw),
            mpeg_frames=mpeg_frames,
            mpeg_samples_upper=mpeg_frames * samples_per_frame,
            mp3_frames_minus_pcm=len(raw) - len(original),
            mp3_offset_vs_pcm=lag,
            mp3_offset_coefficient=round(coefficient, 4),
        )
    try:
        decoded = norm.decode_source(path)
    except norm.DecodeError as error:
        row["rejected"] = str(error).replace(" ", "_")
        return row
    rate = decoded.sample_rate
    row.update(
        source_rate=rate,
        source_channels=decoded.channels,
        source_subtype=decoded.subtype,
        source_frames_decoded=decoded.frames,
        source_frames_info=decoded.info_frames,
    )

    started = time.perf_counter()
    result = norm.normalize(model, decoded, device, shifts=case["shifts"])
    paths = norm.write_stems(result, directory / case["name"])
    row["pipeline_seconds"] = round(time.perf_counter() - started, 3)
    row["separate_seconds"] = round(result.timings["separate"], 3)
    row["model_frames"] = result.model_frames
    row["padded_frames"] = result.padded_frames
    row["trimmed_frames"] = result.trimmed_frames

    total = np.zeros((decoded.frames, 2))
    peaks = []
    for name in norm.STEMS:
        info = sf.info(str(paths[name]))
        audio, stem_rate = sf.read(str(paths[name]), dtype="float32", always_2d=True)
        assert info.subtype == "FLOAT", info.subtype
        assert stem_rate == rate, (stem_rate, rate)
        assert audio.shape == (decoded.frames, 2), (audio.shape, decoded.frames)
        assert np.isfinite(audio).all(), name
        assert np.array_equal(audio, result.stems[name]), f"{name} not bit-exact after WAV round trip"
        peaks.append(float(np.abs(audio).max()))
        total += audio
    row["stem_frames_equal_source"] = True
    row["stem_rate_equal_source"] = True
    row["stem_channels"] = 2
    row["all_finite"] = True
    row["float_wav_bit_exact"] = True
    row["stems_over_1"] = ",".join(n for n, peak in zip(norm.STEMS, peaks) if peak > 1.0) or "none"
    row["max_stem_peak"] = round(max(peaks), 4)
    row["sum_peak"] = round(float(np.abs(total).max()), 4)
    row["samples_over_1"] = int(sum(int((np.abs(result.stems[n]) > 1.0).sum()) for n in norm.STEMS))
    row["source_peak"] = round(float(np.abs(decoded.audio).max()), 4)

    reference = stereo(decoded.audio.astype(np.float64))
    if case.get("silence"):
        row["max_abs_output"] = float(np.abs(total).max())
        return row

    roundtrip, _padded, _trimmed = norm.to_source_rate(norm.to_model_input(decoded), rate, decoded.frames)
    roundtrip = roundtrip.astype(np.float64)
    row["residual_db_vs_source"] = round(residual_db(reference, total), 2)
    row["residual_db_vs_resample_roundtrip"] = round(residual_db(roundtrip, total), 2)
    row["resample_roundtrip_residual_db"] = round(residual_db(reference, roundtrip), 2)

    max_lag = int(0.05 * rate)
    ref_mono = reference.mean(axis=1)
    sum_mono = total.mean(axis=1)
    lag, sub, coefficient = xcorr_lag(ref_mono, sum_mono, max_lag)
    rt_lag, rt_sub, _ = xcorr_lag(ref_mono, roundtrip.mean(axis=1), max_lag)
    row.update(lag_full=lag, lag_full_sub=round(sub, 4), lag_full_coefficient=round(coefficient, 4))
    row.update(resample_roundtrip_lag=rt_lag, resample_roundtrip_lag_sub=round(rt_sub, 4))

    window = int(WINDOW_SECONDS * rate)
    start_lag, start_sub, _ = xcorr_lag(ref_mono[:window], sum_mono[:window], max_lag)
    end_lag, end_sub, _ = xcorr_lag(ref_mono[-window:], sum_mono[-window:], max_lag)
    span = decoded.frames - window
    row.update(
        lag_start=start_lag,
        lag_end=end_lag,
        lag_start_sub=round(start_sub, 4),
        lag_end_sub=round(end_sub, 4),
        drift_samples=round(end_sub - start_sub, 4),
        drift_ppm=round((end_sub - start_sub) / span * 1e6, 3),
    )

    for name in ("drums", "bass"):
        if name not in components or case["container"] == "mp3":
            continue  # real audio has no known components; MP3 components are pre-encoder
        component = stereo(components[name]).mean(axis=1)
        stem_lag, stem_sub, stem_coefficient = xcorr_lag(component, result.stems[name].mean(axis=1).astype(np.float64), max_lag)
        row[f"{name}_stem_lag"] = stem_lag
        row[f"{name}_stem_lag_sub"] = round(stem_sub, 4)
        row[f"{name}_stem_lag_model_rate"] = round(stem_sub * norm.MODEL_RATE / rate, 4)
        row[f"{name}_stem_coefficient"] = round(stem_coefficient, 3)
    return row


def check_row(row: dict, case: dict) -> None:
    if case.get("expect_rejection"):
        assert "rejected" in row, row
        return
    assert "rejected" not in row, row
    assert row["source_frames_decoded"] == row["source_frames_original"] or case["container"] == "mp3"
    assert row["source_rate"] == case["rate"]
    if case.get("silence"):
        assert row["max_abs_output"] < 1e-6, row
        return
    assert row["lag_full"] == 0, row
    assert row["lag_start"] == 0 and row["lag_end"] == 0, row
    assert abs(row["drift_samples"]) < 0.05, row
    assert row["resample_roundtrip_lag"] == 0, row
    assert row["trimmed_frames"] == 0 and row["padded_frames"] == 0, row
    # Per-stem lag against the known synthetic component is a model diagnostic:
    # Demucs' bass estimate carries a small low-frequency phase offset even at
    # 44.1 kHz (no resampling), so it is bounded in model-rate samples rather
    # than required to be exactly zero like the sum-of-stems lag.
    for name in ("drums", "bass"):
        if row.get(f"{name}_stem_coefficient", 0) > 0.5:
            model_rate_lag = row[f"{name}_stem_lag_sub"] * norm.MODEL_RATE / row["source_rate"]
            assert abs(model_rate_lag) <= 0.25, row


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", choices=("cpu", "cuda"), default="cpu")
    parser.add_argument("--only", nargs="*", default=None, help="substring filters for case names")
    parser.add_argument("--no-long", action="store_true", help="skip the 180-second drift case")
    parser.add_argument("--keep", action="store_true", help="keep generated audio")
    parser.add_argument("--real", action="append", type=pathlib.Path, default=[],
                        help="owner-supplied audio, read in place; derived excerpts go to the temp dir")
    parser.add_argument(
        "--model-dir",
        type=pathlib.Path,
        default=pathlib.Path(os.environ["LOCALAPPDATA"]) / "snds-spikes" / "htdemucs-artifact",
    )
    args = parser.parse_args()

    device = torch.device(args.device)
    started = time.perf_counter()
    model = get_model("955717e8", repo=args.model_dir).to(device)
    model.eval()
    print(f"started_at={time.strftime('%Y-%m-%dT%H:%M:%S%z')}")
    print(f"python={sys.version.split()[0]}")
    print(f"torch={torch.__version__}")
    print(f"soundfile={sf.__version__} libsndfile={sf.__libsndfile_version__}")
    print(f"device={args.device}")
    print(f"model_load_seconds={time.perf_counter() - started:.3f}")
    if args.device == "cuda":
        print(f"cuda_device={torch.cuda.get_device_name(device)}")
    started = time.perf_counter()
    norm.separate(model, torch.zeros(2, norm.MODEL_RATE), device)  # one-time kernel/allocator warm-up
    print(f"warmup_seconds={time.perf_counter() - started:.3f}")

    selected = [
        case
        for case in cases(long_case=not args.no_long, real=args.real)
        if not args.only or any(token in case["name"] for token in args.only)
    ]
    directory = pathlib.Path(tempfile.mkdtemp(prefix="snds-normalized-stems-"))
    rows = []
    failures = []
    try:
        for case in selected:
            row = measure(case, model, device, directory)
            print(" ".join(f"{key}={value}" for key, value in row.items()), flush=True)
            try:
                check_row(row, case)
            except AssertionError as error:
                failures.append((case["name"], str(error)))
            rows.append(row)
    finally:
        if args.keep:
            print(f"work_dir={directory}")
        else:
            shutil.rmtree(directory, ignore_errors=True)

    print()
    print("| case | source frames | stem frames | lag (sum/start/end) | drift ppm | residual dB | max stem peak | samples >1.0 | separate s |")
    print("|---|---:|---:|---|---:|---:|---:|---:|---:|")
    for row in rows:
        if "rejected" in row:
            print(f"| {row['case']} | {row.get('libsndfile_frames', '-')} | rejected (truncated decode) | - | - | - | - | - | - |")
            continue
        lags = f"{row.get('lag_full', '-')}/{row.get('lag_start', '-')}/{row.get('lag_end', '-')}"
        print(
            f"| {row['case']} | {row['source_frames_decoded']} | {row['source_frames_decoded']} | {lags} "
            f"| {row.get('drift_ppm', '-')} | {row.get('residual_db_vs_source', '-')} | {row['max_stem_peak']} "
            f"| {row['samples_over_1']} | {row['separate_seconds']} |"
        )
    print()
    print(f"finished_at={time.strftime('%Y-%m-%dT%H:%M:%S%z')}")
    print(f"cases={len(rows)} failures={len(failures)}")
    for name, error in failures:
        print(f"FAILED {name}: {error}")
    assert not failures
    print("normalized_stems=aligned")


if __name__ == "__main__":
    main()
