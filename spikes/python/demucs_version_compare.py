"""Spike: does demucs 4.1.0 reproduce demucs 4.0.1 output for the pinned checkpoint?

Run the same script in two environments sharing the same torch build, then compare:

    <env-4.0.1 python> spikes/python/demucs_version_compare.py run
    uv run --project worker --no-sync python spikes/python/demucs_version_compare.py run
    uv run --project worker --no-sync python spikes/python/demucs_version_compare.py compare

`run` separates a deterministic 20 s synthetic clip on CPU (shifts=0, split=True,
overlap=0.25) and saves stems to spikes/output/demucs_compare/<demucs version>.npy.
It also records whether demucs' stock local-repo loader works on this torch.
"""

from __future__ import annotations

import json
import sys

import numpy as np
from _common import load_htdemucs, load_htdemucs_via_demucs_repo, output_dir, synth_song


def run() -> int:
    import demucs
    import torch
    from demucs.apply import apply_model

    torch.manual_seed(0)
    out_dir = output_dir("demucs_compare")
    info: dict[str, object] = {"demucs": demucs.__version__, "torch": torch.__version__}
    try:
        load_htdemucs_via_demucs_repo()
        info["stock_get_model_repo"] = "ok"
    except Exception as exc:  # noqa: BLE001 - we want to record whatever breaks
        info["stock_get_model_repo"] = f"FAILED: {type(exc).__name__}: {str(exc)[:160]}"

    model = load_htdemucs("cpu")
    mix = torch.from_numpy(synth_song(20.0).T.copy())[None]
    with torch.inference_mode():
        stems = apply_model(model, mix, shifts=0, split=True, overlap=0.25, device="cpu")
    arr = stems[0].numpy()
    np.save(out_dir / f"{demucs.__version__}.npy", arr)
    info["shape"] = list(arr.shape)
    print(json.dumps(info, indent=2))
    return 0


def compare() -> int:
    out_dir = output_dir("demucs_compare")
    a = np.load(out_dir / "4.0.1.npy")
    b = np.load(out_dir / "4.1.0.npy")
    diff = np.abs(a - b)
    ref = np.abs(a).max()
    res = {
        "shape_equal": a.shape == b.shape,
        "max_abs_diff": float(diff.max()),
        "max_rel_to_peak": float(diff.max() / ref),
        "bit_identical": bool(np.array_equal(a, b)),
    }
    print(json.dumps(res, indent=2))
    return 0 if res["shape_equal"] and res["max_rel_to_peak"] < 1e-5 else 1


if __name__ == "__main__":
    sys.exit(run() if sys.argv[1:] == ["run"] else compare())
