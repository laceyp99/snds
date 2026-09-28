"""Spike: NDJSON over pipes, incremental delivery, cancellation, and process-tree kill.

    uv run --project worker --no-sync python spikes/python/transport_parent.py

Plays the role of the C++ app. Scenarios (each prints one JSON line):
  1 incremental      flushed child: every progress line arrives while the child is running
  2 unflushed        child without flush: lines arrive only at exit (why the worker must flush)
  3 cancel_message   {"type":"cancel"} on stdin -> child acknowledges and exits code 4
  4 stdin_eof        parent closes stdin -> child treats it as cancel
  5 kill_child_only  TerminateProcess(child) -> the grandchild SURVIVES (orphan)
  6 kill_job         child in a Job Object (KILL_ON_JOB_CLOSE) -> TerminateJobObject kills
                     child + grandchild
  7 parent_crash     an intermediate "app" process owning the job is killed -> kernel closes
                     the job handle -> worker tree dies (no orphans after an app crash)
  8 hung_worker      child ignores cancel -> grace timeout -> job kill
  9 stderr_undrained parent never reads a flooded stderr pipe -> child blocks (deadlock
                     hazard); draining stderr on a thread fixes it
"""

from __future__ import annotations

import ctypes
import json
import subprocess
import sys
import threading
import time
from ctypes import wintypes
from pathlib import Path

HERE = Path(__file__).resolve().parent
CHILD = str(HERE / "transport_child.py")

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_SET_QUOTA = 0x0100
PROCESS_TERMINATE = 0x0001
STILL_ACTIVE = 259
JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000
JobObjectExtendedLimitInformation = 9


class IO_COUNTERS(ctypes.Structure):
    _fields_ = [
        (n, ctypes.c_ulonglong)
        for n in (
            "ReadOperationCount",
            "WriteOperationCount",
            "OtherOperationCount",
            "ReadTransferCount",
            "WriteTransferCount",
            "OtherTransferCount",
        )
    ]


class JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("PerProcessUserTimeLimit", ctypes.c_int64),
        ("PerJobUserTimeLimit", ctypes.c_int64),
        ("LimitFlags", wintypes.DWORD),
        ("MinimumWorkingSetSize", ctypes.c_size_t),
        ("MaximumWorkingSetSize", ctypes.c_size_t),
        ("ActiveProcessLimit", wintypes.DWORD),
        ("Affinity", ctypes.c_size_t),
        ("PriorityClass", wintypes.DWORD),
        ("SchedulingClass", wintypes.DWORD),
    ]


class JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("BasicLimitInformation", JOBOBJECT_BASIC_LIMIT_INFORMATION),
        ("IoInfo", IO_COUNTERS),
        ("ProcessMemoryLimit", ctypes.c_size_t),
        ("JobMemoryLimit", ctypes.c_size_t),
        ("PeakProcessMemoryUsed", ctypes.c_size_t),
        ("PeakJobMemoryUsed", ctypes.c_size_t),
    ]


k32.CreateJobObjectW.restype = wintypes.HANDLE
k32.CreateJobObjectW.argtypes = [wintypes.LPVOID, wintypes.LPCWSTR]
k32.SetInformationJobObject.argtypes = [
    wintypes.HANDLE,
    ctypes.c_int,
    wintypes.LPVOID,
    wintypes.DWORD,
]
k32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
k32.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
k32.OpenProcess.restype = wintypes.HANDLE
k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
k32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
k32.CloseHandle.argtypes = [wintypes.HANDLE]


def make_kill_on_close_job() -> int:
    job = k32.CreateJobObjectW(None, None)
    if not job:
        raise ctypes.WinError(ctypes.get_last_error())
    info = JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    if not k32.SetInformationJobObject(
        job, JobObjectExtendedLimitInformation, ctypes.byref(info), ctypes.sizeof(info)
    ):
        raise ctypes.WinError(ctypes.get_last_error())
    return job


def assign(job: int, proc: subprocess.Popen) -> None:
    if not k32.AssignProcessToJobObject(job, int(proc._handle)):  # noqa: SLF001
        raise ctypes.WinError(ctypes.get_last_error())


def alive(pid: int) -> bool:
    h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return False
    code = wintypes.DWORD()
    k32.GetExitCodeProcess(h, ctypes.byref(code))
    k32.CloseHandle(h)
    return code.value == STILL_ACTIVE


def force_kill(pid: int) -> None:
    h = k32.OpenProcess(PROCESS_TERMINATE, False, pid)
    if h:
        k32.TerminateProcess(h, 1)
        k32.CloseHandle(h)


class Worker:
    """Popen + reader threads that timestamp every stdout line on arrival."""

    def __init__(self, args: list[str], drain_stderr: bool = True, job: int | None = None):
        self.t0 = time.time()
        self.proc = subprocess.Popen(
            [sys.executable, CHILD, *args],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        if job is not None:
            assign(job, self.proc)
        self.msgs: list[dict] = []
        self.raw_has_crlf = False
        self.stderr_bytes = 0
        self.cv = threading.Condition()
        threading.Thread(target=self._read_stdout, daemon=True).start()
        if drain_stderr:
            threading.Thread(target=self._read_stderr, daemon=True).start()

    def _read_stdout(self) -> None:
        for raw in iter(self.proc.stdout.readline, b""):
            arrived = time.time()
            self.raw_has_crlf |= raw.endswith(b"\r\n")
            msg = json.loads(raw.decode("utf-8"))
            msg["_arrived"] = arrived
            with self.cv:
                self.msgs.append(msg)
                self.cv.notify_all()

    def _read_stderr(self) -> None:
        for raw in iter(self.proc.stderr.readline, b""):
            self.stderr_bytes += len(raw)

    def wait_for(self, pred, timeout: float) -> bool:
        with self.cv:
            return self.cv.wait_for(lambda: pred(self.msgs), timeout)

    def send(self, obj: dict) -> None:
        self.proc.stdin.write((json.dumps(obj) + "\n").encode())
        self.proc.stdin.flush()

    def pid_of(self, typ: str) -> int | None:
        return next((m["pid"] for m in self.msgs if m["type"] == typ), None)


def n_progress(msgs: list[dict]) -> int:
    return sum(m["type"] == "progress" for m in msgs)


def scenario_incremental(flush: bool) -> dict:
    w = Worker(["--steps", "10", "--interval", "0.2"] + ([] if flush else ["--no-flush"]))
    code = w.proc.wait(timeout=30)
    time.sleep(0.2)
    exit_t = time.time()
    prog = [m for m in w.msgs if m["type"] == "progress"]
    lat = [round((m["_arrived"] - m["t"]) * 1000, 1) for m in prog]
    spread = round(prog[-1]["_arrived"] - prog[0]["_arrived"], 2) if prog else None
    return {
        "scenario": "1 incremental" if flush else "2 unflushed",
        "exit_code": code,
        "progress_lines": len(prog),
        "delivery_latency_ms_max": max(lat) if lat else None,
        "arrival_spread_s": spread,  # ~1.8 s if incremental, ~0 if all arrive at exit
        "first_line_before_exit_s": round(exit_t - prog[0]["_arrived"], 2) if prog else None,
        "stdout_has_crlf": w.raw_has_crlf,
    }


def scenario_cancel(kind: str) -> dict:
    w = Worker(["--steps", "100", "--interval", "0.1"])
    w.wait_for(lambda ms: n_progress(ms) >= 3, 10)
    t = time.time()
    if kind == "message":
        w.send({"type": "cancel"})
    else:
        w.proc.stdin.close()
    code = w.proc.wait(timeout=10)
    ack = next((m for m in w.msgs if m["type"] == "cancelled"), None)
    return {
        "scenario": "3 cancel_message" if kind == "message" else "4 stdin_eof",
        "exit_code": code,
        "ack": {k: ack[k] for k in ("reason", "at_step")} if ack else None,
        "cancel_to_exit_ms": round((time.time() - t) * 1000, 1),
    }


def scenario_kill(use_job: bool) -> dict:
    job = make_kill_on_close_job() if use_job else None
    w = Worker(["--steps", "100", "--interval", "0.1", "--spawn-grandchild"], job=job)
    w.wait_for(lambda ms: any(m["type"] == "spawned" for m in ms), 10)
    child, grand = w.proc.pid, w.pid_of("spawned")
    t = time.time()
    if use_job:
        k32.TerminateJobObject(job, 1)
    else:
        w.proc.kill()  # TerminateProcess on the direct child only
    w.proc.wait(timeout=10)
    time.sleep(0.5)
    res = {
        "scenario": "6 kill_job" if use_job else "5 kill_child_only",
        "child_alive_after": alive(child),
        "grandchild_alive_after": alive(grand),
        "kill_ms": round((time.time() - t - 0.5) * 1000, 1),
    }
    if res["grandchild_alive_after"]:
        force_kill(grand)  # clean up the demonstrated orphan
        res["orphan_cleaned_up"] = not alive(grand)
    if job:
        k32.CloseHandle(job)
    return res


APP_SNIPPET = r"""
import sys, json, time
sys.path.insert(0, {here!r})
import transport_parent as tp
job = tp.make_kill_on_close_job()
args = ["--steps", "1000", "--interval", "0.1", "--spawn-grandchild", "--ignore-cancel"]
w = tp.Worker(args, job=job)
w.wait_for(lambda ms: any(m["type"] == "spawned" for m in ms), 10)
print(json.dumps({{"child": w.proc.pid, "grand": w.pid_of("spawned")}}), flush=True)
time.sleep(120)
"""


def scenario_parent_crash() -> dict:
    app = subprocess.Popen(
        [sys.executable, "-c", APP_SNIPPET.format(here=str(HERE))],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        stdin=subprocess.DEVNULL,
    )
    pids = json.loads(app.stdout.readline())
    before = alive(pids["child"]) and alive(pids["grand"])
    app.kill()  # simulated app crash: nobody calls TerminateJobObject
    app.wait(timeout=10)
    time.sleep(0.5)
    res = {
        "scenario": "7 parent_crash",
        "worker_tree_alive_before": before,
        "child_alive_after": alive(pids["child"]),
        "grandchild_alive_after": alive(pids["grand"]),
    }
    for p in (pids["child"], pids["grand"]):
        force_kill(p)
    return res


def scenario_hung(grace: float = 1.0) -> dict:
    job = make_kill_on_close_job()
    w = Worker(["--steps", "1000", "--interval", "0.1", "--ignore-cancel"], job=job)
    w.wait_for(lambda ms: n_progress(ms) >= 2, 10)
    t = time.time()
    w.send({"type": "cancel"})
    try:
        w.proc.wait(timeout=grace)
        escalated = False
    except subprocess.TimeoutExpired:
        k32.TerminateJobObject(job, 1)
        w.proc.wait(timeout=10)
        escalated = True
    k32.CloseHandle(job)
    return {
        "scenario": "8 hung_worker",
        "escalated_to_job_kill": escalated,
        "cancel_to_dead_ms": round((time.time() - t) * 1000, 1),
        "child_alive_after": alive(w.proc.pid),
    }


def scenario_stderr(drain: bool) -> dict:
    job = make_kill_on_close_job()
    w = Worker(
        ["--steps", "3", "--interval", "0.05", "--stderr-flood", "2000000"],
        drain_stderr=drain,
        job=job,
    )
    try:
        code = w.proc.wait(timeout=5)
        blocked = False
    except subprocess.TimeoutExpired:
        code = None
        blocked = True
        k32.TerminateJobObject(job, 1)
        w.proc.wait(timeout=10)
    k32.CloseHandle(job)
    return {
        "scenario": "9 stderr_" + ("drained" if drain else "undrained"),
        "child_blocked": blocked,
        "exit_code": code,
        "progress_lines": n_progress(w.msgs),
    }


def main() -> int:
    rows = [
        scenario_incremental(True),
        scenario_incremental(False),
        scenario_cancel("message"),
        scenario_cancel("eof"),
        scenario_kill(False),
        scenario_kill(True),
        scenario_parent_crash(),
        scenario_hung(),
        scenario_stderr(False),
        scenario_stderr(True),
    ]
    for r in rows:
        print(json.dumps(r))
    by = {r["scenario"]: r for r in rows}
    ok = (
        by["1 incremental"]["arrival_spread_s"] > 1.0
        and by["3 cancel_message"]["exit_code"] == 4
        and by["4 stdin_eof"]["exit_code"] == 4
        and not by["6 kill_job"]["grandchild_alive_after"]
        and not by["7 parent_crash"]["grandchild_alive_after"]
        and not by["8 hung_worker"]["child_alive_after"]
        and not by["9 stderr_drained"]["child_blocked"]
    )
    print(json.dumps({"transport_ok": ok}))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
