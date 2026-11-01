"""Reference stem normalization pipeline for the SNDS normalized-stems spike.

source file -> decode (soundfile, float32) -> model input (44.1 kHz stereo,
mono duplicated) -> Demucs ``apply_model`` -> resample each stem back to the
source rate -> force the exact source frame count -> stereo float32 WAV.

No gain is applied to the output: the mean/std standardization that the
Demucs 4.0.1 CLI performs before inference is inverted exactly afterwards, and
Demucs' ``save_audio`` clip prevention (default ``rescale``) is deliberately
not used. Values above 1.0 are preserved by float32 WAV storage.
"""

from __future__ import annotations

import dataclasses
import pathlib
import random
import time

import julius
import numpy as np
import soundfile as sf
import torch
from demucs.apply import apply_model

MODEL_RATE = 44_100
STEMS = ("drums", "bass", "other", "vocals")
SILENCE_STD = 1e-8


@dataclasses.dataclass
class Decoded:
    audio: np.ndarray  # (frames, channels) float32
    sample_rate: int
    frames: int
    channels: int
    format: str
    subtype: str
    info_frames: int


@dataclasses.dataclass
class Normalized:
    stems: dict[str, np.ndarray]  # name -> (frames, 2) float32
    sample_rate: int
    model_frames: int
    padded_frames: int
    trimmed_frames: int
    timings: dict[str, float]


class DecodeError(ValueError):
    pass


_BITRATES = {
    True: (0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320),
    False: (0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160),
}
_RATES = {3: (44_100, 48_000, 32_000), 2: (22_050, 24_000, 16_000), 0: (11_025, 12_000, 8_000)}


def mpeg_layer3_frames(data: bytes) -> tuple[int, int]:
    """Count MPEG Layer III frames by walking frame headers.

    Returns ``(frame_count, samples_per_frame)``. The count includes a LAME
    Info/Xing frame if present. Free-format streams are not supported.
    """
    pos = 0
    if data[:3] == b"ID3" and len(data) >= 10:
        size = (data[6] & 0x7F) << 21 | (data[7] & 0x7F) << 14 | (data[8] & 0x7F) << 7 | (data[9] & 0x7F)
        pos = 10 + size + (10 if data[5] & 0x10 else 0)
    frames = 0
    samples_per_frame = 0
    while pos + 4 <= len(data):
        header = int.from_bytes(data[pos : pos + 4], "big")
        version = (header >> 19) & 3
        layer = (header >> 17) & 3
        bitrate_index = (header >> 12) & 0xF
        rate_index = (header >> 10) & 3
        valid = (
            (header >> 21) & 0x7FF == 0x7FF
            and version != 1
            and layer == 1
            and bitrate_index not in (0, 15)
            and rate_index != 3
        )
        if not valid:
            if frames == 0:
                pos += 1  # resync before the first frame
                continue
            break  # trailing tags (ID3v1/APE) or garbage
        mpeg1 = version == 3
        bitrate = _BITRATES[mpeg1][bitrate_index] * 1000
        rate = _RATES[version][rate_index]
        pos += (144 if mpeg1 else 72) * bitrate // rate + ((header >> 9) & 1)
        samples_per_frame = 1152 if mpeg1 else 576
        frames += 1
    return frames, samples_per_frame


def check_mp3_complete(path: pathlib.Path, decoded_frames: int) -> int:
    """Reject decodes that stop short of the stream's MPEG frames.

    libsndfile 1.2.0 stops at its own length estimate; for VBR files without
    a Xing/Info header that estimate is derived from the first frame's
    bitrate and silently truncates the audio. A complete decode is at most
    ``frames * spf`` samples and at least that minus the Info frame, encoder
    delay, decoder delay, and padding (bounded here by three frames).
    """
    frames, samples_per_frame = mpeg_layer3_frames(path.read_bytes())
    upper = frames * samples_per_frame
    if not upper or not upper - 3 * samples_per_frame <= decoded_frames <= upper:
        raise DecodeError(
            f"MP3 decode length {decoded_frames} inconsistent with {frames} MPEG frames "
            f"({upper} samples); refusing a possibly truncated decode"
        )
    return upper


def decode_source(path: pathlib.Path) -> Decoded:
    info = sf.info(str(path))
    audio, sample_rate = sf.read(str(path), dtype="float32", always_2d=True)
    if audio.shape[1] not in (1, 2):
        raise DecodeError(f"unsupported channel count {audio.shape[1]}")
    if info.format == "MP3":
        check_mp3_complete(path, audio.shape[0])
    return Decoded(
        audio=np.ascontiguousarray(audio),
        sample_rate=int(sample_rate),
        frames=int(audio.shape[0]),
        channels=int(audio.shape[1]),
        format=info.format,
        subtype=info.subtype,
        info_frames=int(info.frames),
    )


def to_model_input(decoded: Decoded) -> torch.Tensor:
    """Return a (2, model_frames) float32 tensor at 44.1 kHz.

    ``full=True`` rounds the model-rate length up, so the model always sees
    the complete source and the inverse resample never has to invent samples.
    """
    wav = torch.from_numpy(decoded.audio.T.copy())
    if decoded.channels == 1:
        wav = wav.expand(2, -1).contiguous()
    if decoded.sample_rate != MODEL_RATE:
        wav = julius.resample_frac(wav, decoded.sample_rate, MODEL_RATE, full=True)
    return wav.contiguous()


def separate(
    model: torch.nn.Module,
    wav: torch.Tensor,
    device: torch.device,
    shifts: int = 0,
    seed: int = 0,
) -> torch.Tensor:
    """Run Demucs exactly like the 4.0.1 CLI, minus file I/O and clipping."""
    ref = wav.mean(0)
    mean = ref.mean()
    std = ref.std()
    if not torch.isfinite(std) or std < SILENCE_STD:
        std = torch.tensor(1.0)  # digital silence would otherwise divide by zero
    random.seed(seed)  # apply_model's shift trick uses the stdlib RNG
    with torch.inference_mode():
        out = apply_model(
            model,
            ((wav - mean) / std)[None],
            device=device,
            shifts=shifts,
            split=True,
            overlap=0.25,
            progress=False,
        )[0]
    if device.type == "cuda":
        torch.cuda.synchronize(device)
    return (out.cpu() * std + mean).contiguous()


def to_source_rate(stem: torch.Tensor, sample_rate: int, frames: int) -> tuple[np.ndarray, int, int]:
    """Resample one (2, model_frames) stem to the source rate and exact length."""
    if sample_rate != MODEL_RATE:
        max_length = int(np.ceil(stem.shape[-1] * sample_rate / MODEL_RATE))
        stem = julius.resample_frac(
            stem, MODEL_RATE, sample_rate, output_length=min(frames, max_length)
        )
    padded = max(0, frames - stem.shape[-1])
    trimmed = max(0, stem.shape[-1] - frames)
    if padded:
        stem = torch.nn.functional.pad(stem, (0, padded))
    stem = stem[..., :frames]
    return np.ascontiguousarray(stem.numpy().T.astype(np.float32)), padded, trimmed


def normalize(
    model: torch.nn.Module,
    decoded: Decoded,
    device: torch.device,
    shifts: int = 0,
) -> Normalized:
    timings: dict[str, float] = {}
    started = time.perf_counter()
    wav = to_model_input(decoded)
    timings["to_model_rate"] = time.perf_counter() - started

    started = time.perf_counter()
    separated = separate(model, wav, device, shifts=shifts)
    timings["separate"] = time.perf_counter() - started
    assert tuple(model.sources) == STEMS

    started = time.perf_counter()
    stems: dict[str, np.ndarray] = {}
    padded = trimmed = 0
    for index, name in enumerate(STEMS):  # one stem at a time bounds peak memory
        stems[name], padded, trimmed = to_source_rate(
            separated[index], decoded.sample_rate, decoded.frames
        )
    timings["to_source_rate"] = time.perf_counter() - started
    return Normalized(
        stems=stems,
        sample_rate=decoded.sample_rate,
        model_frames=int(wav.shape[-1]),
        padded_frames=padded,
        trimmed_frames=trimmed,
        timings=timings,
    )


def write_stems(normalized: Normalized, out_dir: pathlib.Path) -> dict[str, pathlib.Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    paths = {}
    for name, audio in normalized.stems.items():
        path = out_dir / f"{name}.wav"
        sf.write(str(path), audio, normalized.sample_rate, subtype="FLOAT", format="WAV")
        paths[name] = path
    return paths
