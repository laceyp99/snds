from __future__ import annotations

import argparse
import hashlib
import os
import pathlib

import torch
from demucs.pretrained import get_model


MODEL_NAME = "htdemucs"
MODEL_SIGNATURE = "955717e8"
MODEL_FILENAME = "955717e8-8726e21a.th"
MODEL_URL = (
    "https://dl.fbaipublicfiles.com/demucs/"
    "hybrid_transformer/955717e8-8726e21a.th"
)
EXPECTED_HASH_PREFIX = "8726e21a"
EXPECTED_SHA256 = "8726e21a993978c7ba086d3872e7608d7d5bfca646ca4aca459ffda844faa8b4"


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as model_file:
        for block in iter(lambda: model_file.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def download_model(destination: pathlib.Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    torch.hub.download_url_to_file(
        MODEL_URL,
        str(destination),
        hash_prefix=EXPECTED_HASH_PREFIX,
        progress=True,
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--model-dir",
        type=pathlib.Path,
        default=pathlib.Path(os.environ["LOCALAPPDATA"])
        / "snds-spikes"
        / "htdemucs-artifact",
    )
    parser.add_argument("--device", choices=("cpu", "cuda"), default="cpu")
    args = parser.parse_args()

    model_path = args.model_dir / MODEL_FILENAME
    if not model_path.exists():
        download_model(model_path)

    full_hash = sha256(model_path)
    assert full_hash == EXPECTED_SHA256

    model = get_model(MODEL_SIGNATURE, repo=args.model_dir)
    assert model.sources == ["drums", "bass", "other", "vocals"]
    assert model.samplerate == 44_100
    assert model.audio_channels == 2
    model.to(torch.device(args.device))

    print(f"model_name={MODEL_NAME}")
    print(f"model_signature={MODEL_SIGNATURE}")
    print(f"model_filename={MODEL_FILENAME}")
    print(f"model_sha256={full_hash}")
    print(f"model_bytes={model_path.stat().st_size}")
    print(f"sources={','.join(model.sources)}")
    print(f"samplerate={model.samplerate}")
    print(f"audio_channels={model.audio_channels}")
    print(f"model_device={args.device}")
    print("model_artifact=ready")


if __name__ == "__main__":
    main()
