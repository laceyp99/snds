"""Spike: CUDA detection, intentional CUDA failure, and CPU fallback / explicit CPU retry.

    uv run --project worker --no-sync python spikes/python/cuda_fallback.py

The parent runs one short-lived child worker per scenario (like the real app will) and
reads newline-delimited JSON from its stdout:

  A  auto          CUDA detected + validated by a probe -> separation on cuda
  B  hidden-gpu    CUDA_VISIBLE_DEVICES=-1 (no usable GPU) -> automatic CPU
  C  bad-device    SNDS_CUDA_DEVICE=cuda:7 -> real torch "invalid device ordinal" error in
                   the validation probe -> automatic CPU fallback, reason reported
  D  midjob-fail   SNDS_FAULT_INJECT=cuda_midjob -> CUDA fails after the job started
                   (exit code 3, error code cuda_failed) -> parent performs an EXPLICIT
                   retry with --device cpu, which succeeds
  E  force-cpu     --device cpu with a healthy GPU -> CPU (user "force CPU" setting)

Child mode: cuda_fallback.py child --device auto|cpu|cuda
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import time

EXIT_OK, EXIT_FAILED, EXIT_CUDA_FAILED = 0, 1, 3


def emit(obj: dict) -> None:
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def probe_cuda(device: str) -> str | None:
    """Return None if `device` passes a tiny compute probe, else a reason string."""
    import torch

    try:
        a = torch.randn(256, 256, device=device)
        b = (a @ a).sum().item()
        torch.cuda.synchronize(device)
        if b != b:  # NaN
            return "probe produced NaN"
        return None
    except Exception as exc:  # noqa: BLE001
        return f"{type(exc).__name__}: {str(exc).splitlines()[0]}"


def child(requested: str) -> int:
    import numpy as np
    import torch
    from _common import load_htdemucs, synth_song
    from demucs.apply import apply_model

    cuda_device = os.environ.get("SNDS_CUDA_DEVICE", "cuda:0")
    detected = torch.cuda.is_available()
    emit(
        {
            "type": "compute",
            "requested": requested,
            "cuda_detected": detected,
            "torch_cuda_build": torch.version.cuda,
            "device_count": torch.cuda.device_count() if detected else 0,
        }
    )
    reason = None
    if requested == "cpu":
        device, reason = "cpu", "forced by request"
    elif not detected:
        if requested == "cuda":
            emit({"type": "error", "code": "cuda_unavailable", "retry_on_cpu": True})
            return EXIT_CUDA_FAILED
        device, reason = "cpu", "cuda not available"
    else:
        failure = probe_cuda(cuda_device)
        if failure is None:
            device = cuda_device
        elif requested == "cuda":
            emit({"type": "error", "code": "cuda_failed", "message": failure, "retry_on_cpu": True})
            return EXIT_CUDA_FAILED
        else:
            device, reason = "cpu", f"cuda probe failed: {failure}"
    emit({"type": "device", "device": device, "fallback_reason": reason})

    fault = os.environ.get("SNDS_FAULT_INJECT", "")
    segments_done = 0

    def on_progress(d: dict) -> None:
        nonlocal segments_done
        if d.get("state") != "end":
            return
        segments_done += 1
        emit({"type": "progress", "segments_done": segments_done})
        if fault == "cuda_midjob" and device.startswith("cuda") and segments_done == 2:
            # Stand-in for a device-lost / launch failure surfacing mid-separation.
            raise RuntimeError("CUDA error: unspecified launch failure (injected)")

    try:
        model = load_htdemucs(device)
        mix = torch.from_numpy(synth_song(20.0, seed=5).T.copy())[None]
        t0 = time.perf_counter()
        with torch.inference_mode():
            out = apply_model(
                model, mix, shifts=0, split=True, overlap=0.25, device=device, callback=on_progress
            )
        secs = time.perf_counter() - t0
    except RuntimeError as exc:
        msg = str(exc).splitlines()[0]
        if device.startswith("cuda") and ("CUDA" in msg or "cuda" in msg):
            emit({"type": "error", "code": "cuda_failed", "message": msg, "retry_on_cpu": True})
            return EXIT_CUDA_FAILED
        emit({"type": "error", "code": "separation_failed", "message": msg})
        return EXIT_FAILED
    emit(
        {
            "type": "result",
            "ok": bool(np.isfinite(out.numpy()).all()),
            "device": device,
            "separate_seconds": round(secs, 2),
            "segments": segments_done,
        }
    )
    return EXIT_OK


def run_child(device: str, env_extra: dict[str, str]) -> tuple[int, list[dict]]:
    env = dict(os.environ, **env_extra)
    proc = subprocess.run(
        [sys.executable, "-u", __file__, "child", "--device", device],
        env=env,
        capture_output=True,
        text=True,
        timeout=600,
        check=False,
    )
    msgs = [json.loads(line) for line in proc.stdout.splitlines() if line.strip()]
    return proc.returncode, msgs


def summarize(msgs: list[dict]) -> dict:
    s: dict = {}
    for m in msgs:
        if m["type"] == "device":
            s["device"] = m["device"]
            s["fallback_reason"] = m["fallback_reason"]
        elif m["type"] == "compute":
            s["cuda_detected"] = m["cuda_detected"]
        elif m["type"] == "error":
            s["error"] = {k: m[k] for k in ("code", "message") if k in m}
        elif m["type"] == "result":
            s["result_ok"] = m["ok"]
            s["separate_seconds"] = m["separate_seconds"]
        elif m["type"] == "progress":
            s["progress_msgs"] = s.get("progress_msgs", 0) + 1
    return s


def parent() -> int:
    scenarios = [
        ("A auto", "auto", {}),
        ("B hidden-gpu", "auto", {"CUDA_VISIBLE_DEVICES": "-1"}),
        ("C bad-device", "auto", {"SNDS_CUDA_DEVICE": "cuda:7"}),
        ("D midjob-fail", "auto", {"SNDS_FAULT_INJECT": "cuda_midjob"}),
        ("E force-cpu", "cpu", {}),
    ]
    all_ok = True
    for name, device, env in scenarios:
        code, msgs = run_child(device, env)
        row = {"scenario": name, "exit_code": code, **summarize(msgs)}
        if code == EXIT_CUDA_FAILED:
            # Explicit CPU retry: a *new* worker process, forced to CPU.
            rcode, rmsgs = run_child(
                "cpu", {k: v for k, v in env.items() if k != "SNDS_FAULT_INJECT"}
            )
            row["explicit_cpu_retry"] = {"exit_code": rcode, **summarize(rmsgs)}
            ok = rcode == EXIT_OK and row["explicit_cpu_retry"].get("result_ok", False)
        else:
            ok = code == EXIT_OK and row.get("result_ok", False)
        row["scenario_ok"] = bool(ok)
        all_ok &= bool(ok)
        print(json.dumps(row))
        sys.stdout.flush()
    print(json.dumps({"all_scenarios_ok": all_ok}))
    return 0 if all_ok else 1


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "child":
        dev = sys.argv[sys.argv.index("--device") + 1] if "--device" in sys.argv else "auto"
        sys.exit(child(dev))
    sys.exit(parent())
