"""Spike: htdemucs separation throughput on CPU and CUDA (unknown-cpu-runtime).

    uv run --project worker --no-sync python spikes/python/benchmark_separation.py --device cpu
    uv run --project worker --no-sync python spikes/python/benchmark_separation.py --device cuda

Separates synthetic 44.1 kHz stereo clips (default 30 s and 60 s) with the production
settings proposed in docs/spikes.md (shifts=0, split=True, overlap=0.25, segment=7.8 s).
Reports wall time, real-time factor (processing seconds per audio second) and speed
(audio seconds per processing second), the machine CPU load just before each run and
the share of CPU used by other processes during the run, so contended numbers are visible.
Results are appended to spikes/output/benchmarks.jsonl.
"""

from __future__ import annotations

import argparse
import json
import platform
import subprocess
import sys
import time

from _common import Timer, load_htdemucs, output_dir, synth_song
from _winperf import LoadWindow, cpu_load_percent, logical_cpus, peak_working_set_mib


def physical_cores() -> int:
    try:
        out = subprocess.run(
            [
                "powershell",
                "-NoProfile",
                "-Command",
                "(Get-CimInstance Win32_Processor | Measure-Object NumberOfCores -Sum).Sum",
            ],
            capture_output=True,
            text=True,
            timeout=30,
            check=True,
        ).stdout.strip()
        return int(out)
    except Exception:  # noqa: BLE001
        return logical_cpus()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", choices=["cpu", "cuda"], default="cpu")
    ap.add_argument("--seconds", type=float, nargs="+", default=[30.0, 60.0])
    ap.add_argument("--threads", type=int, default=0, help="0 = physical cores")
    ap.add_argument("--max-busy", type=float, default=15.0, help="wait until load below this")
    ap.add_argument("--wait", type=float, default=300.0, help="max seconds to wait for idle")
    args = ap.parse_args()

    import torch
    from demucs.apply import apply_model

    cores = physical_cores()
    threads = args.threads or cores
    torch.set_num_threads(threads)
    info = {
        "device": args.device,
        "torch": torch.__version__,
        "cpu": platform.processor(),
        "physical_cores": cores,
        "logical_cpus": logical_cpus(),
        "torch_threads": torch.get_num_threads(),
    }
    if args.device == "cuda":
        info["gpu"] = torch.cuda.get_device_name(0)

    with Timer() as t_load:
        model = load_htdemucs(args.device)
    info["model_load_seconds"] = round(t_load.seconds, 2)

    # Warm-up (kernel selection / allocator) on a short clip; not reported as a result.
    warm = torch.from_numpy(synth_song(4.0, seed=7).T.copy())[None]
    with torch.inference_mode():
        apply_model(model, warm, shifts=0, split=True, overlap=0.25, device=args.device)

    results = []
    for secs in args.seconds:
        mix = torch.from_numpy(synth_song(secs, seed=11).T.copy())[None]
        deadline = time.time() + args.wait
        load_before = cpu_load_percent(1.0)
        while load_before > args.max_busy and time.time() < deadline:
            time.sleep(5)
            load_before = cpu_load_percent(1.0)
        if args.device == "cuda":
            torch.cuda.reset_peak_memory_stats()
            torch.cuda.synchronize()
        with LoadWindow() as w, torch.inference_mode():
            stems = apply_model(model, mix, shifts=0, split=True, overlap=0.25, device=args.device)
            if args.device == "cuda":
                torch.cuda.synchronize()
        row = {
            **info,
            "audio_seconds": secs,
            "wall_seconds": round(w.seconds, 2),
            "rtf": round(w.seconds / secs, 3),
            "x_realtime": round(secs / w.seconds, 2),
            "cpu_load_before_pct": load_before,
            "other_process_load_during_pct": w.other_load_pct,
            "contended": load_before > args.max_busy,
            "peak_working_set_mib": peak_working_set_mib(),
            "finite": bool(torch.isfinite(stems).all()),
        }
        if args.device == "cuda":
            row["cuda_peak_alloc_mib"] = round(torch.cuda.max_memory_allocated() / 2**20, 1)
        results.append(row)
        print(json.dumps(row))
        sys.stdout.flush()

    with (output_dir(".") / "benchmarks.jsonl").open("a", encoding="utf-8") as f:
        for row in results:
            f.write(json.dumps(row) + "\n")
    return 0 if all(r["finite"] for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
