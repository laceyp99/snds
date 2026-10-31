"""Source verification and decoding (WAV through libsndfile, MP3 through libsndfile's mpg123).

Supported: WAV with PCM (8/16/24/32-bit) or float (32/64-bit) samples, mono or stereo, 8 kHz to
192 kHz; and MP3, mono or stereo. Everything else is rejected with a stable code and an
actionable message. Decoding is chunked so that it reports progress and polls cancellation.
The MP3 completeness check is the one proven in spikes/normalized-stems.
"""

from __future__ import annotations

import hashlib
import logging
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np
import soundfile as sf

from snds_worker.cancel import CancelToken
from snds_worker.errors import JobFailure

log = logging.getLogger(__name__)

MIN_SAMPLE_RATE = 8_000
MAX_SAMPLE_RATE = 192_000
MAX_DURATION_SECONDS = 30 * 60
SUPPORTED_CHANNELS = (1, 2)
WAV_FORMATS = ("WAV", "WAVEX", "RF64")
WAV_SUBTYPES = ("PCM_U8", "PCM_S8", "PCM_16", "PCM_24", "PCM_32", "FLOAT", "DOUBLE")

_HASH_BLOCK = 4 * 1024 * 1024
_READ_FRAMES = 1 << 20
# Share of the "decoding" stage spent hashing the source; the rest is decoding.
HASH_SHARE = 0.25

Progress = Callable[[float], None]


@dataclass
class DecodedSource:
    audio: np.ndarray  # (frames, channels) float32
    sample_rate: int
    channels: int
    frames: int
    container: str  # "wav" or "mp3"
    subtype: str

    @property
    def duration_seconds(self) -> float:
        return self.frames / self.sample_rate


def verify_source(
    path: Path, expected_size: int, expected_sha256: str, token: CancelToken, progress: Progress
) -> None:
    """Checks the size, then the SHA-256 of the whole file, against the request.

    Raises JobFailure source_unreadable (cannot be opened or read) or source_changed.
    """
    try:
        size = path.stat().st_size
    except OSError as error:
        raise JobFailure(
            "source_unreadable",
            f"The source file cannot be read: {error.strerror or error}.",
            details=f"{type(error).__name__}: {error}",
            suggested_action="relocate_source",
        ) from error

    if size != expected_size:
        raise JobFailure(
            "source_changed",
            f"The source file changed since it was imported ({size} bytes instead of "
            f"{expected_size}). Import it again.",
        )

    digest = hashlib.sha256()
    done = 0
    try:
        with path.open("rb") as file:
            while block := file.read(_HASH_BLOCK):
                token.raise_if_cancelled()
                digest.update(block)
                done += len(block)
                progress(done / max(size, 1))
    except OSError as error:
        raise JobFailure(
            "source_unreadable",
            f"The source file cannot be read: {error.strerror or error}.",
            details=f"{type(error).__name__}: {error}",
            suggested_action="relocate_source",
        ) from error

    if done != size or digest.hexdigest() != expected_sha256:
        raise JobFailure(
            "source_changed",
            "The source file's contents changed since it was imported. Import it again.",
            details=f"expected SHA-256 {expected_sha256}, read {digest.hexdigest()}",
        )


# --- MP3 completeness (spikes/normalized-stems) --------------------------------------------

_BITRATES = {
    True: (0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320),
    False: (0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160),
}
_RATES = {3: (44_100, 48_000, 32_000), 2: (22_050, 24_000, 16_000), 0: (11_025, 12_000, 8_000)}


def mpeg_layer3_frames(data: bytes) -> tuple[int, int]:
    """Counts MPEG Layer III frames by walking frame headers.

    Returns (frame_count, samples_per_frame). The count includes a LAME Info/Xing frame if
    present. Free-format streams are not supported.
    """
    pos = 0
    if data[:3] == b"ID3" and len(data) >= 10:
        size = (
            (data[6] & 0x7F) << 21
            | (data[7] & 0x7F) << 14
            | (data[8] & 0x7F) << 7
            | (data[9] & 0x7F)
        )
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


def check_mp3_complete(path: Path, decoded_frames: int) -> None:
    """Rejects decodes that stop short of the stream's MPEG frames.

    libsndfile stops at its own length estimate; for VBR files without a Xing/Info header that
    estimate comes from the first frame's bitrate and silently truncates the audio. A complete
    decode is at most frames * samples_per_frame and at least that minus three frames (Info
    frame, encoder and decoder delay, padding).
    """
    frames, samples_per_frame = mpeg_layer3_frames(path.read_bytes())
    upper = frames * samples_per_frame
    if not upper or not upper - 3 * samples_per_frame <= decoded_frames <= upper:
        raise JobFailure(
            "decode_failed",
            "The MP3 could not be decoded completely (its length is inconsistent). Re-encode "
            "it, or convert it to WAV.",
            details=(
                f"decoded {decoded_frames} frames; the stream has {frames} MPEG frames "
                f"({upper} samples)"
            ),
        )


# --- Decoding ------------------------------------------------------------------------------


def _unsupported(message: str, details: str | None = None) -> JobFailure:
    return JobFailure("source_unsupported", message, details=details)


def inspect_source(path: Path) -> tuple[str, Any]:
    """Validates the container, sample format, channel layout and rate; returns the container."""
    try:
        info = sf.info(str(path))
    except (sf.LibsndfileError, RuntimeError) as error:
        raise _unsupported(
            "This file is not a WAV or MP3 that SNDS can read. Convert it to WAV or MP3.",
            details=str(error),
        ) from error

    if info.format in WAV_FORMATS:
        container = "wav"
        if info.subtype not in WAV_SUBTYPES:
            raise _unsupported(
                f"This WAV uses the sample format {info.subtype}, which is not supported. "
                "Use PCM or float samples.",
            )
    elif info.format == "MP3":
        container = "mp3"
    else:
        raise _unsupported(
            f"{info.format} files are not supported. Convert the song to WAV or MP3."
        )

    if info.channels not in SUPPORTED_CHANNELS:
        raise _unsupported(
            f"This file has {info.channels} channels; only mono and stereo are supported."
        )
    if not MIN_SAMPLE_RATE <= info.samplerate <= MAX_SAMPLE_RATE:
        raise _unsupported(
            f"This file has a {info.samplerate} Hz sample rate; "
            f"{MIN_SAMPLE_RATE} to {MAX_SAMPLE_RATE} Hz is supported."
        )
    if info.frames > info.samplerate * MAX_DURATION_SECONDS:
        raise _unsupported("This file exceeds 30 minutes. Trim it to 30 minutes or less.")
    return container, info


def decode_source(path: Path, token: CancelToken, progress: Progress) -> DecodedSource:
    """Decodes the whole file to float32 (frames, channels). Raises JobFailure on any problem."""
    container, info = inspect_source(path)

    blocks: list[np.ndarray] = []
    total = 0
    expected = max(int(info.frames), 1)
    try:
        with sf.SoundFile(str(path)) as file:
            while True:
                token.raise_if_cancelled()
                block = file.read(_READ_FRAMES, dtype="float32", always_2d=True)
                if not len(block):
                    break
                blocks.append(block)
                total += len(block)
                progress(min(1.0, total / expected))
    except OSError as error:
        raise JobFailure(
            "source_unreadable",
            f"The source file cannot be read: {error.strerror or error}.",
            details=f"{type(error).__name__}: {error}",
            suggested_action="relocate_source",
        ) from error
    except (sf.LibsndfileError, RuntimeError) as error:
        raise JobFailure(
            "decode_failed",
            "The audio is corrupt or could not be decoded.",
            details=str(error),
        ) from error

    if not total:
        raise JobFailure("decode_failed", "The file contains no audio.")

    if container == "wav" and total != info.frames:
        raise JobFailure(
            "decode_failed",
            "The WAV file is truncated or inconsistent.",
            details=f"decoded {total} frames; the header declares {info.frames}",
        )
    if container == "mp3":
        check_mp3_complete(path, total)

    audio = np.concatenate(blocks) if len(blocks) > 1 else blocks[0]
    del blocks
    if not np.isfinite(audio).all():
        raise JobFailure("decode_failed", "The audio contains invalid (NaN or infinite) samples.")

    log.info(
        "Decoded %s: %d frames, %d Hz, %d channel(s), %s",
        container,
        total,
        info.samplerate,
        info.channels,
        info.subtype,
    )
    return DecodedSource(
        audio=np.ascontiguousarray(audio),
        sample_rate=int(info.samplerate),
        channels=int(info.channels),
        frames=int(total),
        container=container,
        subtype=info.subtype,
    )
