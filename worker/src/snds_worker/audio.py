"""Source decoding, input validation and the resampling helpers used for normalization.

Decoding uses soundfile / libsndfile (MP3 through its bundled mpg123). The decoded frame count
is the truth: MP3 header frame counts are estimates for files without a Xing/LAME tag.
"""

from __future__ import annotations

import math
import struct
from dataclasses import dataclass, field
from fractions import Fraction
from pathlib import Path
from typing import Any

import numpy as np
import soundfile as sf
from scipy.signal import resample_poly

from snds_worker.protocol import ErrorCode, WorkerError

MODEL_SR = 44100
MIN_SAMPLERATE = 8000
MAX_SAMPLERATE = 192000
MAX_SOURCE_SECONDS = 30 * 60  # also keeps every float32 stem below the 4 GiB WAV limit
PCM_WAV_SUBTYPES = frozenset({"PCM_U8", "PCM_S8", "PCM_16", "PCM_24", "PCM_32", "FLOAT", "DOUBLE"})
SUPPORTED_FORMATS: dict[str, frozenset[str]] = {
    "WAV": PCM_WAV_SUBTYPES,
    "WAVEX": PCM_WAV_SUBTYPES,
    "MP3": frozenset({"MPEG_LAYER_III"}),
}
RESAMPLER = {"name": "scipy.signal.resample_poly", "window": ["kaiser", 5.0], "zero_phase": True}


@dataclass
class DecodedSource:
    audio: np.ndarray | None  # (frames, channels) float32; released after separation starts
    samplerate: int
    channels: int
    format: str
    subtype: str
    header_frames: int
    frames: int
    mp3: dict[str, Any] | None = field(default=None)

    def describe(self) -> dict[str, Any]:
        return {
            "format": self.format,
            "subtype": self.subtype,
            "samplerate": self.samplerate,
            "channels": self.channels,
            "header_frames": self.header_frames,
            "decoded_frames": self.frames,
            "duration_seconds": round(self.frames / self.samplerate, 6),
            "decoder": {"name": "libsndfile", "version": sf.__libsndfile_version__},
            "mp3": self.mp3,
        }


def probe_source(path: Path) -> sf._SoundFileInfo:
    """Header-level checks before decoding. Raises WorkerError with an input error code."""
    try:
        info = sf.info(str(path))
    except sf.LibsndfileError as exc:
        raise WorkerError(
            ErrorCode.UNSUPPORTED_FORMAT,
            "The file is not a readable WAV or MP3 file.",
            {"path": str(path), "error": str(exc)},
        ) from exc
    except OSError as exc:
        raise WorkerError(
            ErrorCode.SOURCE_UNREADABLE, f"Cannot read the source: {exc}", {"path": str(path)}
        ) from exc
    subtypes = SUPPORTED_FORMATS.get(info.format)
    if subtypes is None or info.subtype not in subtypes:
        raise WorkerError(
            ErrorCode.UNSUPPORTED_FORMAT,
            f"Unsupported audio format {info.format}/{info.subtype}. SNDS accepts PCM or float "
            "WAV and MP3.",
            {"format": info.format, "subtype": info.subtype},
        )
    if info.channels not in (1, 2):
        raise WorkerError(
            ErrorCode.UNSUPPORTED_CHANNEL_COUNT,
            f"The source has {info.channels} channels; only mono and stereo are supported.",
            {"channels": info.channels},
        )
    if not MIN_SAMPLERATE <= info.samplerate <= MAX_SAMPLERATE:
        raise WorkerError(
            ErrorCode.UNSUPPORTED_SAMPLE_RATE,
            f"The sample rate {info.samplerate} Hz is outside "
            f"{MIN_SAMPLERATE}-{MAX_SAMPLERATE} Hz.",
            {"samplerate": info.samplerate},
        )
    # Header frame counts of tagless MP3s are estimates: allow slack, re-check after decoding.
    if info.frames > MAX_SOURCE_SECONDS * info.samplerate * 1.05:
        raise _too_long(info.frames, info.samplerate)
    return info


def _too_long(frames: int, samplerate: int) -> WorkerError:
    return WorkerError(
        ErrorCode.SOURCE_TOO_LONG,
        f"The source is longer than {MAX_SOURCE_SECONDS // 60} minutes.",
        {"seconds": round(frames / samplerate, 1), "max_seconds": MAX_SOURCE_SECONDS},
    )


def decode_source(path: Path) -> DecodedSource:
    info = probe_source(path)
    try:
        audio, sr = sf.read(str(path), dtype="float32", always_2d=True)
    except (sf.LibsndfileError, RuntimeError, ValueError) as exc:
        raise WorkerError(
            ErrorCode.DECODE_FAILED, f"Decoding the source failed: {exc}", {"path": str(path)}
        ) from exc
    except MemoryError as exc:
        raise WorkerError(
            ErrorCode.OUT_OF_MEMORY, "Not enough memory to decode the source"
        ) from exc
    if audio.shape[0] == 0:
        raise WorkerError(ErrorCode.SOURCE_EMPTY, "The source contains no audio frames.")
    if audio.shape[0] > MAX_SOURCE_SECONDS * sr:
        raise _too_long(audio.shape[0], sr)
    if not np.isfinite(audio).all():
        # Float WAVs can carry NaN/Inf; separation would propagate them into every stem.
        raise WorkerError(
            ErrorCode.DECODE_FAILED, "The source contains non-finite (NaN/Inf) samples."
        )
    mp3 = None
    if info.format == "MP3":
        try:
            mp3 = parse_mp3_info(path)
        except Exception as exc:  # informational only
            mp3 = {"parse_error": str(exc)}
    return DecodedSource(
        audio=audio,
        samplerate=int(sr),
        channels=int(audio.shape[1]),
        format=info.format,
        subtype=info.subtype,
        header_frames=int(info.frames),
        frames=int(audio.shape[0]),
        mp3=mp3,
    )


def to_stereo(x: np.ndarray) -> np.ndarray:
    """(frames, 1|2) -> (frames, 2). Mono is duplicated (the model wants stereo)."""
    if x.shape[1] == 2:
        return x
    return np.repeat(x, 2, axis=1)


def resample(x: np.ndarray, sr_in: int, sr_out: int, axis: int = 0) -> np.ndarray:
    """Zero-phase polyphase FIR (Kaiser 5.0) at the exact reduced ratio.
    Output length is ceil(n * up / down)."""
    if sr_in == sr_out:
        return x
    r = Fraction(sr_out, sr_in)
    return resample_poly(x, r.numerator, r.denominator, axis=axis).astype(np.float32, copy=False)


def fit_length(x: np.ndarray, frames: int, axis: int = 0) -> np.ndarray:
    """Trim or zero-pad ``axis`` to exactly ``frames``."""
    n = x.shape[axis]
    if n == frames:
        return x
    if n > frames:
        index = [slice(None)] * x.ndim
        index[axis] = slice(0, frames)
        return x[tuple(index)]
    pad = [(0, 0)] * x.ndim
    pad[axis] = (0, frames - n)
    return np.pad(x, pad)


def peak_dbfs(x: np.ndarray) -> float | None:
    """Peak level in dBFS (can exceed 0 for float data); None for digital silence."""
    if x.size == 0:
        return None
    peak = float(np.max(np.abs(x)))
    if peak <= 0.0 or not math.isfinite(peak):
        return None
    return round(20.0 * math.log10(peak), 2)


def rms_dbfs(x: np.ndarray) -> float | None:
    if x.size == 0:
        return None
    ms = float(np.mean(np.square(x, dtype=np.float64)))
    if ms <= 0.0:
        return None
    return 10.0 * math.log10(ms)


# --- MP3 gapless information (for the app's Original A/B alignment) ----------------------

_BITRATES_V1_L3 = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320]
_BITRATES_V2_L3 = [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160]
_SR_TABLE = {3: [44100, 48000, 32000], 2: [22050, 24000, 16000], 0: [11025, 12000, 8000]}


def parse_mp3_info(path: Path, max_bytes: int = 256 << 20) -> dict[str, Any]:
    """Count MPEG layer III frames and read the Xing/Info + LAME tag (encoder delay/padding).

    ``gapless_frames`` = audio frames x samples per frame - delay - padding, which is what a
    gapless decoder (libsndfile/mpg123) returns. Decoders that ignore the tag start 529 + delay
    samples late (see docs/spikes.md).
    """
    data = path.read_bytes()[:max_bytes]
    pos = 0
    id3 = 0
    if data[:3] == b"ID3" and len(data) >= 10:
        s = data[6:10]
        id3 = 10 + ((s[0] << 21) | (s[1] << 14) | (s[2] << 7) | s[3])
        pos = id3
    audio_frames = 0
    spf = 0
    mpeg_sr = None
    info_tag: dict[str, Any] | None = None
    first = True
    while pos + 4 <= len(data):
        h = struct.unpack(">I", data[pos : pos + 4])[0]
        ver = (h >> 19) & 3
        layer = (h >> 17) & 3
        br_i = (h >> 12) & 0xF
        sr_i = (h >> 10) & 3
        if (h >> 21) & 0x7FF != 0x7FF or ver == 1 or layer != 1 or br_i in (0, 15) or sr_i == 3:
            pos += 1
            continue
        pad = (h >> 9) & 1
        mode = (h >> 6) & 3
        prot = not ((h >> 16) & 1)
        sr = _SR_TABLE[ver][sr_i]
        br = (_BITRATES_V1_L3 if ver == 3 else _BITRATES_V2_L3)[br_i] * 1000
        frame_spf = 1152 if ver == 3 else 576
        length = (144 if ver == 3 else 72) * br // sr + pad
        if first:
            first = False
            side = (32 if mode != 3 else 17) if ver == 3 else (17 if mode != 3 else 9)
            off = pos + 4 + (2 if prot else 0) + side
            tag = data[off : off + 4]
            if tag in (b"Xing", b"Info"):
                flags = struct.unpack(">I", data[off + 4 : off + 8])[0]
                p = off + 8
                for bit, size in ((1, 4), (2, 4), (4, 100), (8, 4)):
                    if flags & bit:
                        p += size
                enc = data[p : p + 9]
                delay = padding = None
                if enc[:4] in (b"LAME", b"Lavf", b"Lavc", b"L3.9"):
                    d = data[p + 21 : p + 24]
                    if len(d) == 3:
                        delay = (d[0] << 4) | (d[1] >> 4)
                        padding = ((d[1] & 0xF) << 8) | d[2]
                info_tag = {
                    "tag": tag.decode(),
                    "encoder": enc.decode("latin-1").strip("\x00 "),
                    "encoder_delay": delay,
                    "padding": padding,
                }
                pos += max(length, 1)
                continue
        audio_frames += 1
        spf = frame_spf
        mpeg_sr = sr
        pos += max(length, 1)
    out: dict[str, Any] = {
        "id3v2_bytes": id3,
        "mpeg_samplerate": mpeg_sr,
        "samples_per_frame": spf,
        "audio_frames": audio_frames,
        "raw_frames": audio_frames * spf,
        "info_tag": info_tag,
        "gapless_info": bool(info_tag and info_tag.get("encoder_delay") is not None),
    }
    if out["gapless_info"]:
        out["gapless_frames"] = (
            audio_frames * spf - info_tag["encoder_delay"] - info_tag["padding"]  # type: ignore[index]
        )
    return out
