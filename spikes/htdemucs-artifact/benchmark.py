from __future__ import annotations

import argparse
import os
import pathlib
import time

import torch
from demucs.apply import apply_model
from demucs.pretrained import get_model


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", choices=("cpu", "cuda"), required=True)
    parser.add_argument("--duration", type=float, default=2.0)
    parser.add_argument(
        "--model-dir",
        type=pathlib.Path,
        default=pathlib.Path(os.environ["LOCALAPPDATA"])
        / "snds-spikes"
        / "htdemucs-artifact",
    )
    args = parser.parse_args()

    device = torch.device(args.device)
    model = get_model("955717e8", repo=args.model_dir).to(device)
    frames = round(args.duration * model.samplerate)
    timeline = torch.arange(frames, dtype=torch.float32) / model.samplerate
    mixture = torch.stack(
        (
            0.1 * torch.sin(2 * torch.pi * 220 * timeline),
            0.1 * torch.sin(2 * torch.pi * 330 * timeline),
        )
    ).unsqueeze(0)

    started = time.perf_counter()
    with torch.inference_mode():
        output = apply_model(
            model,
            mixture,
            device=device,
            shifts=0,
            split=True,
            overlap=0.25,
            progress=False,
        )
    if args.device == "cuda":
        torch.cuda.synchronize()
    elapsed = time.perf_counter() - started

    assert output.shape == (1, 4, 2, frames)
    assert torch.isfinite(output).all()
    print(f"device={args.device}")
    print(f"input_seconds={args.duration:.3f}")
    print(f"output_shape={tuple(output.shape)}")
    print(f"elapsed_seconds={elapsed:.3f}")
    print("inference_smoke=ready")


if __name__ == "__main__":
    main()
