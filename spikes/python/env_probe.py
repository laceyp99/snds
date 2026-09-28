"""Spike: report the worker environment and run a tiny torch smoke test.

Run (after `uv sync --project worker --extra cpu|cuda --frozen`):
    uv run --project worker --no-sync python spikes/python/env_probe.py

Prints one JSON object. Exit code 0 only if every required check passed.
"""

from __future__ import annotations

import json
import platform
import sys
import time


def main() -> int:
    report: dict[str, object] = {
        "python": sys.version.split()[0],
        "executable": sys.executable,
        "platform": platform.platform(),
    }
    ok = True

    import numpy as np
    import scipy
    import soundfile as sf

    report["numpy"] = np.__version__
    report["scipy"] = scipy.__version__
    report["soundfile"] = sf.__version__
    report["libsndfile"] = sf.__libsndfile_version__
    formats = sf.available_formats()
    report["libsndfile_mp3"] = "MP3" in formats
    report["libsndfile_mpeg_subtypes"] = sorted(
        s for s in sf.available_subtypes("MP3") if s.startswith("MPEG")
    )

    import torch

    report["torch"] = torch.__version__
    report["torch_cuda_build"] = torch.version.cuda
    report["torch_cudnn"] = torch.backends.cudnn.version() if torch.cuda.is_available() else None
    report["torch_num_threads"] = torch.get_num_threads()

    x = torch.randn(512, 512)
    t0 = time.perf_counter()
    y = x @ x
    report["cpu_matmul_ms"] = round((time.perf_counter() - t0) * 1000, 3)
    ok &= bool(torch.isfinite(y).all())

    report["cuda_available"] = torch.cuda.is_available()
    if torch.cuda.is_available():
        props = torch.cuda.get_device_properties(0)
        report["cuda_device"] = props.name
        report["cuda_capability"] = f"{props.major}.{props.minor}"
        report["cuda_total_mem_gib"] = round(props.total_memory / 2**30, 2)
        report["cuda_arch_list"] = torch.cuda.get_arch_list()
        xg = x.to("cuda")
        torch.cuda.synchronize()
        t0 = time.perf_counter()
        yg = xg @ xg
        torch.cuda.synchronize()
        report["cuda_matmul_ms"] = round((time.perf_counter() - t0) * 1000, 3)
        max_err = float((yg.cpu() - y).abs().max() / y.abs().max())
        report["cuda_vs_cpu_rel_err"] = max_err
        ok &= max_err < 1e-3

    import demucs
    import demucs.apply
    import demucs.htdemucs
    import demucs.pretrained  # noqa: F401  (import check only)

    report["demucs"] = demucs.__version__
    # torchaudio is intentionally absent: SNDS does its own audio I/O.
    try:
        import torchaudio  # noqa: F401

        report["torchaudio_present"] = True
    except ImportError:
        report["torchaudio_present"] = False

    report["ok"] = bool(ok)
    print(json.dumps(report, indent=2))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
