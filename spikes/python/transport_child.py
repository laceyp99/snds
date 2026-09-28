"""Worker emulator for the transport spike (see transport_parent.py).

Protocol sketch (NOT the final schema): newline-delimited JSON, one object per line.
  stdout: machine-readable messages only (hello, progress, spawned, cancelled, result)
  stderr: human-readable logs only
  stdin : control messages, e.g. {"type": "cancel"}; EOF on stdin == cancel

Flags:
  --steps N --interval S   emit N progress lines, S seconds apart
  --no-flush               write with Python's default buffering (demonstrates the problem)
  --spawn-grandchild       start a sleeping grandchild process (process-tree kill test)
  --ignore-cancel          never react to cancel (simulates a hung worker)
  --stderr-flood BYTES     write BYTES of log text to stderr before starting
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import threading
import time

EXIT_OK, EXIT_CANCELLED = 0, 4


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--steps", type=int, default=10)
    ap.add_argument("--interval", type=float, default=0.2)
    ap.add_argument("--no-flush", action="store_true")
    ap.add_argument("--spawn-grandchild", action="store_true")
    ap.add_argument("--ignore-cancel", action="store_true")
    ap.add_argument("--stderr-flood", type=int, default=0)
    args = ap.parse_args()

    if not args.no_flush:
        # The recommended worker setup: UTF-8, LF-only line endings, flush every line.
        sys.stdout.reconfigure(encoding="utf-8", newline="\n", line_buffering=True)
        sys.stderr.reconfigure(encoding="utf-8", line_buffering=True)

    def emit(obj: dict) -> None:
        obj["t"] = time.time()
        sys.stdout.write(json.dumps(obj, ensure_ascii=True) + "\n")
        if not args.no_flush:
            sys.stdout.flush()

    cancel = threading.Event()
    cancel_reason = {"why": None}

    def stdin_reader() -> None:
        for line in sys.stdin:
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                print(f"ignoring malformed control line: {line!r}", file=sys.stderr)
                continue
            if msg.get("type") == "cancel":
                cancel_reason["why"] = "cancel_message"
                cancel.set()
                return
        cancel_reason["why"] = "stdin_eof"  # parent closed stdin or died
        cancel.set()

    threading.Thread(target=stdin_reader, daemon=True).start()

    if args.stderr_flood:
        chunk = "log line padding padding padding padding padding padding padding\n"
        written = 0
        while written < args.stderr_flood:
            sys.stderr.write(chunk)
            written += len(chunk)
        sys.stderr.flush()

    emit({"type": "hello", "pid": os.getpid()})
    grandchild = None
    if args.spawn_grandchild:
        grandchild = subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(120)"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        emit({"type": "spawned", "pid": grandchild.pid})

    for i in range(1, args.steps + 1):
        if cancel.is_set() and not args.ignore_cancel:
            if grandchild is not None:
                grandchild.kill()
            emit({"type": "cancelled", "reason": cancel_reason["why"], "at_step": i - 1})
            return EXIT_CANCELLED
        time.sleep(args.interval)  # stand-in for a chunk of separation work
        emit({"type": "progress", "step": i, "of": args.steps})
        print(f"step {i} done", file=sys.stderr)

    emit({"type": "result", "ok": True})
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
