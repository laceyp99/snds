"""Spike: MP3 encode/decode behaviour without FFmpeg (soundfile / libsndfile + mpg123).

    uv run --project worker --no-sync python spikes/python/mp3_probe.py

For each synthetic source (known exact frame count) and each encoder:
  * `soundfile` -> libsndfile's LAME encoder (format MP3, subtype MPEG_LAYER_III)
  * `lameenc`   -> raw LAME frames (installed as a demucs dependency)
the script decodes with soundfile (libsndfile's bundled mpg123) and records:
  * sf.info() frame count vs. actually decoded frames vs. the original frame count
  * cross-correlation lag of the decode against the original (leading offset)
  * parsed Xing/Info + LAME tag: encoder delay, padding, MPEG frame count
  * the raw (non-gapless) length = audio MPEG frames x samples-per-frame, which is what a
    decoder that ignores the LAME tag (possibly JUCE's MP3 reader) would produce.
Fixtures and a manifest are written to spikes/output/mp3/ for the C++ A/B spike.
"""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

import numpy as np
import soundfile as sf
from _common import output_dir, synth_song
from normalize_stems import xcorr_lag

SOURCES = [
    ("44k1_stereo_10s", 44100, 2, 441000),
    ("44k1_stereo_odd", 44100, 2, 441000 + 12345),
    ("48k_stereo_odd", 48000, 2, 480000 + 777),
    ("44k1_mono_odd", 44100, 1, 441000 + 3),
]

BITRATES_V1_L3 = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320]
BITRATES_V2_L3 = [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160]
SR_TABLE = {3: [44100, 48000, 32000], 2: [22050, 24000, 16000], 0: [11025, 12000, 8000]}


def parse_mp3(path: Path) -> dict:
    data = path.read_bytes()
    pos = 0
    id3 = 0
    if data[:3] == b"ID3":
        size = data[6:10]
        id3 = 10 + ((size[0] << 21) | (size[1] << 14) | (size[2] << 7) | size[3])
        pos = id3
    frames = []
    info_tag = None
    while pos + 4 <= len(data):
        h = struct.unpack(">I", data[pos : pos + 4])[0]
        if (h >> 21) & 0x7FF != 0x7FF:
            pos += 1
            continue
        ver = (h >> 19) & 3
        layer = (h >> 17) & 3
        br_i = (h >> 12) & 0xF
        sr_i = (h >> 10) & 3
        pad = (h >> 9) & 1
        mode = (h >> 6) & 3
        prot = not ((h >> 16) & 1)
        if ver == 1 or layer != 1 or br_i in (0, 15) or sr_i == 3:
            pos += 1
            continue
        sr = SR_TABLE[ver][sr_i]
        br = (BITRATES_V1_L3 if ver == 3 else BITRATES_V2_L3)[br_i] * 1000
        spf = 1152 if ver == 3 else 576
        length = (144 if ver == 3 else 72) * br // sr + pad
        if not frames:
            side = (32 if mode != 3 else 17) if ver == 3 else (17 if mode != 3 else 9)
            off = pos + 4 + (2 if prot else 0) + side
            tag = data[off : off + 4]
            if tag in (b"Xing", b"Info"):
                flags = struct.unpack(">I", data[off + 4 : off + 8])[0]
                p = off + 8
                nframes = None
                if flags & 1:
                    nframes = struct.unpack(">I", data[p : p + 4])[0]
                    p += 4
                if flags & 2:
                    p += 4
                if flags & 4:
                    p += 100
                if flags & 8:
                    p += 4
                enc = data[p : p + 9]
                delay = padding = None
                if enc[:4] in (b"LAME", b"Lavf", b"Lavc", b"L3.9"):
                    d = data[p + 21 : p + 24]
                    delay = (d[0] << 4) | (d[1] >> 4)
                    padding = ((d[1] & 0xF) << 8) | d[2]
                info_tag = {
                    "tag": tag.decode(),
                    "frames_field": nframes,
                    "encoder": enc.decode("latin-1").strip("\x00 "),
                    "encoder_delay": delay,
                    "padding": padding,
                }
                frames.append(("info", spf, sr, br))
                pos += length
                continue
        frames.append(("audio", spf, sr, br))
        pos += length
    audio = [f for f in frames if f[0] == "audio"]
    spf = audio[0][1] if audio else 0
    res = {
        "bytes": len(data),
        "id3v2_bytes": id3,
        "mpeg_sample_rate": audio[0][2] if audio else None,
        "samples_per_frame": spf,
        "audio_frames": len(audio),
        "bitrates_kbps": sorted({f[3] // 1000 for f in audio}),
        "info_tag": info_tag,
        "raw_samples_audio_frames": len(audio) * spf,
        "raw_samples_including_info_frame": len(frames) * spf,
    }
    if info_tag and info_tag["encoder_delay"] is not None:
        res["gapless_samples"] = len(audio) * spf - info_tag["encoder_delay"] - info_tag["padding"]
    return res


def encode_lameenc(x: np.ndarray, sr: int, path: Path, kbps: int = 192) -> None:
    import lameenc

    enc = lameenc.Encoder()
    enc.set_bit_rate(kbps)
    enc.set_in_sample_rate(sr)
    enc.set_channels(x.shape[1])
    enc.set_quality(2)
    pcm = (np.clip(x, -1, 1) * 32767).astype("<i2").tobytes()
    path.write_bytes(enc.encode(pcm) + enc.flush())


def encode_soundfile(x: np.ndarray, sr: int, path: Path) -> None:
    sf.write(path, x, sr, format="MP3", subtype="MPEG_LAYER_III")


def main() -> int:
    out = output_dir("mp3")
    manifest = []
    for name, sr, ch, frames in SOURCES:
        x = synth_song(frames / sr + 0.01, sr=sr, channels=ch, seed=99 + sr + ch)[:frames]
        sf.write(out / f"{name}.wav", x, sr, subtype="PCM_16")
        for enc_name, enc in (("soundfile", encode_soundfile), ("lameenc", encode_lameenc)):
            path = out / f"{name}.{enc_name}.mp3"
            try:
                enc(x, sr, path)
            except Exception as exc:  # noqa: BLE001
                manifest.append({"file": path.name, "encode_error": repr(exc)})
                continue
            info = sf.info(str(path))
            y, sr_dec = sf.read(str(path), dtype="float32", always_2d=True)
            a = x.mean(axis=1).astype(np.float64)
            b = y.mean(axis=1).astype(np.float64)
            n = min(len(a), len(b))
            lag, lag_frac = xcorr_lag(a[:n], b[:n], max_lag=4096)
            parsed = parse_mp3(path)
            row = {
                "file": path.name,
                "encoder": enc_name,
                "source_wav": f"{name}.wav",
                "sample_rate": sr,
                "channels": ch,
                "original_frames": frames,
                "sf_info_frames": info.frames,
                "sf_decoded_frames": y.shape[0],
                "sf_decoded_channels": y.shape[1],
                "sf_decoded_sr": sr_dec,
                "decoded_minus_original": y.shape[0] - frames,
                "decode_lag_vs_original": lag,
                "decode_lag_subsample": round(lag_frac, 3),
                "libsndfile": sf.__libsndfile_version__,
                **{f"mp3_{k}": v for k, v in parsed.items()},
            }
            manifest.append(row)
            print(json.dumps(row))
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
