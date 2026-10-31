"""Parent-side harness for the worker transport spike (Windows only).

Launches ``worker.py`` through ``winjob.py`` (CreateProcessW suspended + Job
Object + pipes, like the planned C++ launcher), drives every scenario several
times, asserts the protocol rules, and prints ``key=value`` lines and timing
tables. Standard library only; the worker interpreter is configurable.

    python harness.py --worker-python PATH [--repeat N] [--only name ...]
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import statistics
import sys
import tempfile
import time

import winjob

HERE = pathlib.Path(__file__).resolve().parent
EXIT_CODES = {"result": 0, "invalid_request": 10, "unsupported_protocol_version": 11, "cancelled": 12,
              "job_failed": 13, "internal_error": 14}
KILLED_BY_PARENT = 99
UNICODE_DIR = "Ünïcødé 曲 \U0001f3b5"  # Ünïcødé 曲 🎵 (includes a non-BMP char)


class Run:
    """One worker launch plus everything the parent observed."""

    def __init__(self, python: str, root: pathlib.Path, use_job: bool = True, extra_args: list[str] | None = None):
        argv = [python, "-X", "utf8", str(HERE / "worker.py"), *(extra_args or [])]
        self.child = winjob.launch(argv, use_job=use_job)
        self.messages: list[tuple[float, dict]] = []
        self.root = root
        self.malformed: list[bytes] = []

    def send(self, payload: dict | bytes) -> float:
        data = payload if isinstance(payload, bytes) else (json.dumps(payload, ensure_ascii=True) + "\n").encode("ascii")
        assert self.child.write(data), "write to worker stdin failed"
        return time.perf_counter()

    def pump(self, until=None, timeout: float = 30.0) -> dict | None:
        """Read stdout lines until ``until(message)`` is true, EOF, or timeout."""
        deadline = time.perf_counter() + timeout
        while time.perf_counter() < deadline:
            try:
                arrived, line = self.child.stdout.lines.get(timeout=0.01)
            except Exception:  # queue.Empty
                if self.child.stdout.eof_at is not None and self.child.stdout.lines.empty():
                    return None
                continue
            try:
                message = json.loads(line.decode("ascii"))
                assert isinstance(message, dict) and message.get("protocol") == 1
            except Exception:
                self.malformed.append(line)
                continue
            self.messages.append((arrived, message))
            if until and until(message):
                return message
        return None

    def drain(self, timeout: float = 30.0) -> None:
        self.pump(timeout=timeout)

    def terminal(self) -> dict | None:
        for _arrived, message in reversed(self.messages):
            if message["type"] in ("result", "failure", "cancelled"):
                return message
        return None

    def first(self, kind: str) -> tuple[float, dict] | None:
        return next(((t, m) for t, m in self.messages if m["type"] == kind), None)

    def check_stream(self) -> None:
        assert not self.malformed, f"non-protocol stdout lines: {self.malformed[:3]}"
        seqs = [message["seq"] for _t, message in self.messages]
        assert seqs == list(range(1, len(seqs) + 1)), f"sequence gap/duplication: {seqs[:10]}..."
        terminals = [m for _t, m in self.messages if m["type"] in ("result", "failure", "cancelled")]
        assert len(terminals) <= 1, "more than one terminal message"
        if terminals:
            assert self.messages[-1][1] is terminals[0], "messages after the terminal message"

    def classify(self, killed: bool = False) -> str:
        code = self.child.exit_code()
        terminal = self.terminal()
        if killed:
            return "killed_by_parent" if code == KILLED_BY_PARENT else f"killed_unexpected_code_{code}"
        if terminal is None:
            return f"crashed_exit_0x{code:08X}" if code else "protocol_violation_exit_0_without_terminal"
        key = terminal["type"] if terminal["type"] != "failure" else terminal["code"]
        expected = EXIT_CODES.get(key, EXIT_CODES["job_failed"])
        if code != expected:
            return f"protocol_violation_{key}_exit_{code}"
        return key

    def finish(self) -> None:
        """Parent cleanup: wait for exit, close the job (kills leftovers), drain stderr."""
        self.child.wait(10)
        self.child.close_job()
        self.child.stderr.join(5)
        self.child.close()


def make_request(root: pathlib.Path, **simulate) -> dict:
    folder = root / UNICODE_DIR
    folder.mkdir(parents=True, exist_ok=True)
    source = folder / "source ü 曲.wav"
    source.write_bytes(b"RIFF")
    return {
        "protocol": 1,
        "type": "job",
        "job_id": f"job-{time.perf_counter_ns()}",
        "input": str(source),
        "output_dir": str(folder / "stems ✓"),
        "simulate": simulate,
    }


def wait_dead(pids_handles: dict[int, int], timeout: float) -> float | None:
    """Wait until every handle is signaled; return seconds taken or None."""
    started = time.perf_counter()
    for handle in pids_handles.values():
        remaining = max(0.0, timeout - (time.perf_counter() - started))
        if winjob.WaitForSingleObject(handle, int(remaining * 1000)) != winjob.WAIT_OBJECT_0:
            return None
    return time.perf_counter() - started


def open_all(pids: list[int]) -> dict[int, int]:
    handles = {}
    for pid in pids:
        handle = winjob.open_process(pid)
        if handle:
            handles[pid] = handle
    return handles


def close_all(handles: dict[int, int]) -> None:
    for handle in handles.values():
        winjob.CloseHandle(handle)


# --------------------------------------------------------------------------
# Scenarios: each returns a dict of metrics and raises AssertionError on failure
# --------------------------------------------------------------------------


def scenario_happy_unicode(python: str, root: pathlib.Path) -> dict:
    run = Run(python, root)
    request = make_request(root, mode="work", steps=20, step_ms=50, chatter=True, grandchild=True)
    sent = run.send(request)
    run.drain()
    exited = run.child.wait(10)
    exit_at = time.perf_counter()
    pids = run.child.job_pids()
    images = sorted(pathlib.Path(winjob.process_image(pid)).name for pid in pids)
    run.check_stream()
    outcome = run.classify()
    assert exited and outcome == "result", outcome
    result = run.terminal()
    assert result["echo"]["input"] == request["input"], "input path did not round-trip"
    assert result["echo"]["output_dir"] == request["output_dir"], "output path did not round-trip"
    assert pathlib.Path(result["outputs"]["drums"]).is_file(), "output file missing at non-ASCII path"
    progress = [t for t, m in run.messages if m["type"] == "progress"]
    stdout_eof = run.child.stdout.eof_at
    grandchild = next(m["grandchild_pid"] for _t, m in run.messages if m["type"] == "diagnostic")
    accepted = run.first("accepted")[1]
    run.finish()
    stderr = run.child.stderr.raw.decode("utf-8", errors="replace")
    for marker in ("python print()", "warnings.warn", "os.write(1)", "STD_OUTPUT_HANDLE", "ucrtbase puts",
                   "chatter: tqdm", "grandchild alive", "Ünïcødé 曲"):
        assert marker in stderr, f"chatter marker {marker!r} not found on stderr"
    return {
        "outcome": outcome,
        "launched_pid": run.child.pid,
        "interpreter_pid": accepted["pid"],
        "interpreter_ppid": accepted["ppid"],
        "job_pids_at_exit": len(pids),
        "job_images_at_exit": ",".join(images),
        "grandchild_in_job": grandchild in pids,
        "first_message_ms": (run.messages[0][0] - run.child.launched_at) * 1000,
        "request_to_accepted_ms": (run.first("accepted")[0] - sent) * 1000,
        "progress_messages": len(progress),
        "progress_span_ms": (progress[-1] - progress[0]) * 1000,
        "first_progress_before_exit_ms": (exit_at - progress[0]) * 1000,
        "stdout_eof_before_job_close": stdout_eof is not None,
        "msvcrt_printf_seen_on_stderr": "legacy msvcrt printf" in stderr,
        "stdout_lines": len(run.messages),
    }


def scenario_request_file(python: str, root: pathlib.Path) -> dict:
    request = make_request(root, mode="work", steps=3, step_ms=10)
    request_path = root / UNICODE_DIR / "request 曲.json"
    request_path.write_text(json.dumps(request, ensure_ascii=True), encoding="ascii")
    run = Run(python, root, extra_args=["--request-file", str(request_path)])
    run.drain()
    run.child.wait(10)
    run.check_stream()
    outcome = run.classify()
    assert outcome == "result", outcome
    assert run.terminal()["echo"]["input"] == request["input"]
    run.finish()
    return {"outcome": outcome, "first_message_ms": (run.messages[0][0] - run.child.launched_at) * 1000}


def _cancel(python: str, root: pathlib.Path, how: str, **simulate) -> dict:
    run = Run(python, root)
    run.send(make_request(root, **simulate))
    seen = 0

    def third_progress(message: dict) -> bool:
        nonlocal seen
        seen += message["type"] == "progress"
        return seen >= 3

    assert run.pump(third_progress, timeout=60), "worker never reported progress"
    if how == "message":
        sent = run.send({"protocol": 1, "type": "cancel"})
    else:
        run.child.close_stdin()
        sent = time.perf_counter()
    ack = run.pump(lambda m: m["type"] == "cancelled", timeout=10)
    ack_at = time.perf_counter()
    exited = run.child.wait(10)
    exit_at = time.perf_counter()
    run.drain(timeout=5)
    run.check_stream()
    outcome = run.classify()
    assert ack is not None and exited and outcome == "cancelled", outcome
    reason = ack["reason"]
    run.finish()
    return {"outcome": outcome, "reason": reason, "cancel_ack_ms": (ack_at - sent) * 1000,
            "cancel_exit_ms": (exit_at - sent) * 1000}


def scenario_cancel_message(python: str, root: pathlib.Path) -> dict:
    return _cancel(python, root, "message", mode="work", steps=400, step_ms=50, grandchild=True)


def scenario_cancel_stdin_close(python: str, root: pathlib.Path) -> dict:
    return _cancel(python, root, "close", mode="work", steps=400, step_ms=50)


def scenario_cancel_during_torch(python: str, root: pathlib.Path) -> dict:
    return _cancel(python, root, "message", mode="torch", steps=200, matmuls=10)


def scenario_blocking_read_hazard(python: str, root: pathlib.Path, stall: float = 6.0) -> dict:
    """Regression: a pending blocking stdin read stalls DLL loading (import torch)."""
    run = Run(python, root)
    sent = run.send(make_request(root, mode="torch", steps=200, matmuls=10, control="blocking"))
    progressed = run.pump(lambda m: m["type"] == "progress", timeout=stall)
    assert progressed is None, "hazard not reproduced: torch imported while a read was pending"
    cancel_at = run.send({"protocol": 1, "type": "cancel"})
    ack = run.pump(lambda m: m["type"] == "cancelled", timeout=30)
    run.child.wait(10)
    run.drain(timeout=5)
    run.check_stream()
    outcome = run.classify()
    assert ack is not None and outcome == "cancelled", outcome
    run.finish()
    return {"outcome": "hazard_reproduced", "stalled_seconds": cancel_at - sent,
            "unblocked_by_stdin_ms": (time.perf_counter() - cancel_at) * 1000}


def scenario_hard_cancel_hung(python: str, root: pathlib.Path, grace: float = 1.0) -> dict:
    run = Run(python, root)
    run.send(make_request(root, mode="hang_gil", hang_ms=120_000, grandchild=True))
    assert run.pump(lambda m: m["type"] == "progress", timeout=30), "no progress before hang"
    sent = run.send({"protocol": 1, "type": "cancel"})
    ack = run.pump(lambda m: m["type"] == "cancelled", timeout=grace)
    assert ack is None, "hung worker unexpectedly acknowledged cancel"
    pids = run.child.job_pids()
    handles = open_all(pids)
    started = time.perf_counter()
    run.child.terminate_job(KILLED_BY_PARENT)
    worker_dead = run.child.wait(10)
    worker_ms = (time.perf_counter() - started) * 1000
    tree = wait_dead(handles, 10)
    tree_ms = (time.perf_counter() - started) * 1000
    active = run.child.job_active()
    stdout_eof = run.child.stdout.join(5) or run.child.stdout.eof_at is not None
    run.drain(timeout=1)
    run.check_stream()
    outcome = run.classify(killed=True)
    close_all(handles)
    assert worker_dead and tree is not None and active == 0 and outcome == "killed_by_parent", outcome
    run.finish()
    return {"outcome": outcome, "graceful_wait_ms": (started - sent) * 1000, "job_processes_killed": len(pids),
            "terminate_to_worker_exit_ms": worker_ms, "terminate_to_tree_dead_ms": tree_ms,
            "stdout_eof_after_kill": stdout_eof}


def scenario_parent_crash_job_close(python: str, root: pathlib.Path) -> dict:
    run = Run(python, root)
    run.send(make_request(root, mode="work", steps=400, step_ms=50, grandchild=True))
    assert run.pump(lambda m: m["type"] == "diagnostic", timeout=30)
    assert run.pump(lambda m: m["type"] == "progress", timeout=30)
    pids = run.child.job_pids()
    handles = open_all(pids)
    started = time.perf_counter()
    run.child.close_job()  # what the OS does to the app's handles when the app dies
    tree = wait_dead(handles, 10)
    tree_ms = (time.perf_counter() - started) * 1000
    close_all(handles)
    assert tree is not None, "job close did not kill the tree"
    run.child.wait(5)
    run.finish()
    return {"outcome": "tree_killed_on_job_close", "job_processes_killed": len(pids), "job_close_to_tree_dead_ms": tree_ms}


def _expect(python: str, root: pathlib.Path, payload, expected: str, close_stdin: bool = False) -> dict:
    run = Run(python, root)
    if payload is not None:
        run.send(payload)
    if close_stdin:
        run.child.close_stdin()
    run.drain(timeout=60)
    run.child.wait(10)
    run.check_stream()
    outcome = run.classify()
    assert outcome == expected or outcome.startswith(expected), f"{outcome} != {expected}"
    terminal = run.terminal() or {}
    code = run.child.exit_code()
    run.finish()
    return {"outcome": outcome, "exit_code": f"0x{code:08X}" if code > 255 else code,
            "failure_code": terminal.get("code", "-"),
            "supported_protocols": ",".join(map(str, terminal.get("supported_protocols", []))) or "-"}


def scenario_malformed_json(python: str, root: pathlib.Path) -> dict:
    return _expect(python, root, b'{"protocol": 1, "type": "job", \n', "invalid_request")


def scenario_unsupported_protocol(python: str, root: pathlib.Path) -> dict:
    return _expect(python, root, dict(make_request(root), protocol=99), "unsupported_protocol_version")


def scenario_missing_field(python: str, root: pathlib.Path) -> dict:
    request = make_request(root)
    del request["output_dir"]
    return _expect(python, root, request, "invalid_request")


def scenario_empty_stdin(python: str, root: pathlib.Path) -> dict:
    return _expect(python, root, None, "invalid_request", close_stdin=True)


def scenario_job_failure(python: str, root: pathlib.Path) -> dict:
    return _expect(python, root, make_request(root, mode="fail"), "separation_failed")


def scenario_internal_exception(python: str, root: pathlib.Path) -> dict:
    return _expect(python, root, make_request(root, mode="exception"), "internal_error")


def scenario_native_crash(python: str, root: pathlib.Path) -> dict:
    return _expect(python, root, make_request(root, mode="crash", steps=10, step_ms=20), "crashed_exit_0x")


def scenario_abort(python: str, root: pathlib.Path) -> dict:
    return _expect(python, root, make_request(root, mode="abort", steps=10, step_ms=20), "crashed_exit_0x")


def scenario_exit_without_result(python: str, root: pathlib.Path) -> dict:
    return _expect(python, root, make_request(root, mode="exit_silently", steps=10, step_ms=20),
                   "protocol_violation_exit_0_without_terminal")


def scenario_burst_and_big_result(python: str, root: pathlib.Path) -> dict:
    run = Run(python, root)
    started = run.send(make_request(root, mode="burst", messages=20_000, big_result_kb=1024))
    run.drain(timeout=120)
    run.child.wait(10)
    elapsed = time.perf_counter() - started
    run.check_stream()
    outcome = run.classify()
    assert outcome == "result", outcome
    payload = len(run.terminal()["payload"])
    reads = run.child.stdout.read_calls
    run.finish()
    return {"outcome": outcome, "messages": len(run.messages), "result_payload_bytes": payload,
            "stdout_read_calls": reads, "messages_per_second": len(run.messages) / elapsed}


def scenario_direct_kill_negative_control(python: str, root: pathlib.Path) -> dict:
    """No Job Object: TerminateProcess on the launched process only (juce::ChildProcess::kill)."""
    run = Run(python, root, use_job=False)
    run.send(make_request(root, mode="work", steps=400, step_ms=50, grandchild=True))
    assert run.pump(lambda m: m["type"] == "diagnostic", timeout=30)
    accepted = run.first("accepted")[1]
    grandchild = next(m["grandchild_pid"] for _t, m in run.messages if m["type"] == "diagnostic")
    interpreter = accepted["pid"]
    watch = {pid: winjob.open_process(pid, terminate=True) for pid in {interpreter, grandchild}}
    winjob.TerminateProcess(run.child.process, KILLED_BY_PARENT)
    run.child.wait(5)
    time.sleep(1.0)
    survivors = {pid: winjob.process_alive(handle) for pid, handle in watch.items() if handle}
    stdout_eof = run.child.stdout.eof_at is not None
    for handle in watch.values():  # clean up whatever survived
        if handle:
            winjob.TerminateProcess(handle, 1)
            winjob.WaitForSingleObject(handle, 5000)
            winjob.CloseHandle(handle)
    run.child.stdout.join(5)
    run.child.stderr.join(5)
    run.child.close()
    return {
        "outcome": "observed",
        "launched_is_interpreter": run.child.pid == interpreter,
        "interpreter_survived": survivors.get(interpreter, False),
        "grandchild_survived": survivors.get(grandchild, False),
        "stdout_eof_within_1s": stdout_eof,
    }


SCENARIOS = {name.removeprefix("scenario_"): func for name, func in globals().items() if name.startswith("scenario_")}
TIMING_KEYS = ("first_message_ms", "request_to_accepted_ms", "cancel_ack_ms", "cancel_exit_ms",
               "terminate_to_worker_exit_ms", "terminate_to_tree_dead_ms", "job_close_to_tree_dead_ms",
               "messages_per_second", "first_progress_before_exit_ms")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--worker-python", default=sys.executable)
    parser.add_argument("--repeat", type=int, default=5)
    parser.add_argument("--only", nargs="*")
    args = parser.parse_args()

    print(f"harness_python={sys.version.split()[0]}")
    print(f"worker_python={args.worker_python}")
    root = pathlib.Path(tempfile.mkdtemp(prefix="snds-transport-"))
    failures = []
    summary: dict[str, dict[str, list]] = {}
    try:
        for name, func in SCENARIOS.items():
            if args.only and name not in args.only:
                continue
            for index in range(args.repeat):
                case_root = root / f"{name}-{index}"
                try:
                    metrics = func(args.worker_python, case_root)
                except AssertionError as error:
                    failures.append(f"{name}[{index}]: {error}")
                    print(f"scenario={name} run={index} FAILED {error}")
                    continue
                print(f"scenario={name} run={index} " + " ".join(
                    f"{k}={v:.1f}" if isinstance(v, float) else f"{k}={v}" for k, v in metrics.items()))
                for key, value in metrics.items():
                    summary.setdefault(name, {}).setdefault(key, []).append(value)
    finally:
        shutil.rmtree(root, ignore_errors=True)

    print()
    print("| scenario | runs | outcome | timing (median / max) |")
    print("|---|---:|---|---|")
    for name, metrics in summary.items():
        outcomes = sorted(set(map(str, metrics.get("outcome", []))))
        timings = []
        for key in TIMING_KEYS:
            values = metrics.get(key)
            if values:
                timings.append(f"{key} {statistics.median(values):.1f} / {max(values):.1f}")
        print(f"| {name} | {len(metrics.get('outcome', []))} | {', '.join(outcomes)} | {'; '.join(timings) or '-'} |")
    print()
    print(f"failures={len(failures)}")
    for failure in failures:
        print(f"FAILED {failure}")
    assert not failures
    print("worker_transport=ready")


if __name__ == "__main__":
    main()
