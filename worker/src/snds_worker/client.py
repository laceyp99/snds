"""Reference client for the worker protocol (what the C++ app does, minus the Job Object).

    python -m snds_worker.client separate <source.wav|mp3> [--device auto|cpu|cuda]
        [--data-root DIR] [--cancel-at-stage STAGE [--cancel-delay S] [--cancel-mode message|eof]]
    python -m snds_worker.client readiness [--force-cpu]

It computes the source fingerprint like the app, spawns
``<python> -u -X utf8 -m snds_worker <command>``, writes the request line, keeps stdin open as
the control channel, drains stderr continuously, parses stdout NDJSON and checks that the last
line is a terminal message whose ``exit_code`` agrees with the process exit code.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import queue
import subprocess
import sys
import threading
import time
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from snds_worker.protocol import PROTOCOL_VERSION, TERMINAL_TYPES


def fingerprint(path: Path) -> tuple[str, int]:
    h = hashlib.sha256()
    size = 0
    with path.open("rb") as f:
        while block := f.read(1 << 20):
            h.update(block)
            size += len(block)
    return h.hexdigest(), size


def make_request(
    source: Path, job_id: str | None = None, device: str = "auto", data_root: Path | None = None
) -> dict[str, Any]:
    sha, size = fingerprint(source)
    req: dict[str, Any] = {
        "type": "separate",
        "protocol_version": PROTOCOL_VERSION,
        "job_id": job_id or uuid.uuid4().hex,
        "source": {"path": str(source.resolve()), "sha256": sha, "size_bytes": size},
        "options": {"device": device},
    }
    if data_root is not None:
        req["data_root"] = str(data_root)
    return req


@dataclass
class RunResult:
    exit_code: int
    messages: list[dict[str, Any]]
    stderr: str
    protocol_errors: list[str] = field(default_factory=list)
    elapsed: float = 0.0

    @property
    def terminal(self) -> dict[str, Any] | None:
        return (
            self.messages[-1]
            if self.messages and self.messages[-1]["type"] in TERMINAL_TYPES
            else None
        )

    def of_type(self, type_: str) -> list[dict[str, Any]]:
        return [m for m in self.messages if m["type"] == type_]

    @property
    def consistent(self) -> bool:
        """Exactly one terminal message, last, with an exit_code equal to the process's."""
        terminals = [m for m in self.messages if m["type"] in TERMINAL_TYPES]
        return (
            not self.protocol_errors
            and len(terminals) == 1
            and self.terminal is not None
            and self.terminal.get("exit_code") == self.exit_code
        )


class WorkerProcess:
    def __init__(
        self,
        command: str,
        args: list[str] | None = None,
        *,
        python: str | None = None,
        env: dict[str, str] | None = None,
    ) -> None:
        argv = [
            python or sys.executable,
            "-u",
            "-X",
            "utf8",
            "-m",
            "snds_worker",
            command,
            *(args or []),
        ]
        full_env = dict(os.environ)
        full_env.update(env or {})
        full_env["PYTHONUNBUFFERED"] = "1"
        full_env["PYTHONUTF8"] = "1"
        flags = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
        self.proc = subprocess.Popen(
            argv,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=full_env,
            creationflags=flags,
        )
        self.messages: queue.Queue = queue.Queue()
        self.stderr_lines: list[str] = []
        self.protocol_errors: list[str] = []
        self._out = threading.Thread(target=self._read_stdout, daemon=True)
        self._err = threading.Thread(target=self._read_stderr, daemon=True)
        self._out.start()
        self._err.start()

    def _read_stdout(self) -> None:
        assert self.proc.stdout is not None
        for raw in self.proc.stdout:
            line = raw.rstrip(b"\n").rstrip(b"\r")
            if raw.endswith(b"\r\n"):
                self.protocol_errors.append("CRLF line ending")
            if not line:
                continue
            try:
                msg = json.loads(line.decode("utf-8"))
                if not isinstance(msg, dict) or "type" not in msg:
                    raise ValueError("not a message object")
            except (ValueError, UnicodeDecodeError) as exc:
                self.protocol_errors.append(f"malformed line {line[:120]!r}: {exc}")
                continue
            self.messages.put(msg)
        self.messages.put(None)

    def _read_stderr(self) -> None:
        assert self.proc.stderr is not None
        for raw in self.proc.stderr:  # always drained: a full stderr pipe blocks the worker
            self.stderr_lines.append(raw.decode("utf-8", "replace").rstrip())

    def send(self, obj: dict[str, Any]) -> None:
        assert self.proc.stdin is not None
        try:
            self.proc.stdin.write((json.dumps(obj) + "\n").encode("utf-8"))
            self.proc.stdin.flush()
        except OSError:
            pass

    def close_stdin(self) -> None:
        try:
            if self.proc.stdin is not None:
                self.proc.stdin.close()
        except OSError:
            pass

    def kill(self) -> None:
        self.proc.kill()


def run_worker(
    command: str,
    args: list[str] | None = None,
    *,
    request: dict[str, Any] | str | None = None,
    env: dict[str, str] | None = None,
    python: str | None = None,
    cancel_at_stage: str | None = None,
    cancel_delay: float = 0.0,
    cancel_mode: str = "message",
    close_stdin_after_request: bool = False,
    timeout: float = 600.0,
    on_message: Any = None,
) -> RunResult:
    t0 = time.perf_counter()
    wp = WorkerProcess(command, args, python=python, env=env)
    if request is not None:
        assert wp.proc.stdin is not None
        line = request if isinstance(request, str) else json.dumps(request)
        wp.proc.stdin.write((line + "\n").encode("utf-8"))
        wp.proc.stdin.flush()
    if close_stdin_after_request or request is None:
        wp.close_stdin()
    messages: list[dict[str, Any]] = []
    cancel_timer: threading.Timer | None = None
    deadline = time.monotonic() + timeout
    while True:
        try:
            msg = wp.messages.get(timeout=max(0.1, deadline - time.monotonic()))
        except queue.Empty:
            wp.kill()
            break
        if msg is None:
            break
        messages.append(msg)
        if on_message is not None:
            on_message(msg)
        if (
            cancel_at_stage
            and cancel_timer is None
            and msg["type"] == "stage"
            and msg["stage"] == cancel_at_stage
        ):

            def fire() -> None:
                if cancel_mode == "eof":
                    wp.close_stdin()
                else:
                    wp.send({"type": "cancel", "protocol_version": PROTOCOL_VERSION})

            cancel_timer = threading.Timer(cancel_delay, fire)
            cancel_timer.start()
        if time.monotonic() > deadline:
            wp.kill()
            break
    try:
        code = wp.proc.wait(timeout=max(1.0, deadline - time.monotonic()))
    except subprocess.TimeoutExpired:
        wp.kill()
        code = wp.proc.wait()
    if cancel_timer is not None:
        cancel_timer.cancel()
    wp.close_stdin()
    wp._err.join(timeout=5)
    return RunResult(
        exit_code=code,
        messages=messages,
        stderr="\n".join(wp.stderr_lines),
        protocol_errors=wp.protocol_errors,
        elapsed=time.perf_counter() - t0,
    )


def _print(msg: dict[str, Any]) -> None:
    t = msg["type"]
    if t == "progress":
        d = msg.get("detail") or {}
        extra = (
            f" {d.get('segments_done')}/{d.get('segments_total')}" if "segments_done" in d else ""
        )
        print(
            f"  progress {msg['stage']:<18} {msg['stage_progress']:6.1%}"
            f" overall {msg['overall']:6.1%}{extra}"
        )
    elif t == "stage":
        print(f"stage {msg['stage_index'] + 1}/{msg['stage_count']} {msg['stage']}: {msg['label']}")
    else:
        body = {k: v for k, v in msg.items() if k not in ("protocol_version", "seq", "ts")}
        print(json.dumps(body)[:600])
    sys.stdout.flush()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="python -m snds_worker.client")
    sub = ap.add_subparsers(dest="command", required=True)
    s = sub.add_parser("separate")
    s.add_argument("source", type=Path)
    s.add_argument("--device", default="auto", choices=["auto", "cpu", "cuda"])
    s.add_argument("--data-root", type=Path, default=None)
    s.add_argument("--job-id", default=None)
    s.add_argument("--cancel-at-stage", default=None)
    s.add_argument("--cancel-delay", type=float, default=0.0)
    s.add_argument("--cancel-mode", default="message", choices=["message", "eof"])
    r = sub.add_parser("readiness")
    r.add_argument("--force-cpu", action="store_true")
    r.add_argument("--data-root", type=Path, default=None)
    args = ap.parse_args(argv)
    if args.command == "separate":
        req = make_request(args.source, args.job_id, args.device, args.data_root)
        print(f"request: {json.dumps(req)}")
        res = run_worker(
            "separate",
            request=req,
            cancel_at_stage=args.cancel_at_stage,
            cancel_delay=args.cancel_delay,
            cancel_mode=args.cancel_mode,
            on_message=_print,
        )
    else:
        extra = ["--force-cpu"] if args.force_cpu else []
        if args.data_root:
            extra += ["--data-root", str(args.data_root)]
        res = run_worker(
            "readiness", ["--protocol-version", str(PROTOCOL_VERSION), *extra], on_message=_print
        )
    print(
        f"exit code {res.exit_code}, {len(res.messages)} messages, {res.elapsed:.2f} s, "
        f"consistent={res.consistent}, protocol_errors={res.protocol_errors}"
    )
    return res.exit_code


if __name__ == "__main__":
    raise SystemExit(main())
