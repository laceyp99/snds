"""Spike worker for the SNDS NDJSON-over-pipes transport.

Protocol (version 1, spike shape):

* The parent writes exactly one JSON request line to stdin and keeps stdin
  open. Later stdin lines are control messages (``{"type":"cancel"}``).
  stdin EOF also means cancel (the parent went away or gave up).
  ``--request-file PATH`` is supported for comparison only.
* The worker writes NDJSON messages to the *original* stdout handle, one
  ``json.dumps(..., ensure_ascii=True)`` line per message, unbuffered.
  Everything else (print, warnings, tqdm, native writes to fd 1 or
  STD_OUTPUT_HANDLE, grandchildren) is routed to stderr.
* Terminal messages: ``result`` (exit 0), ``failure`` (exit 10/11/13/14), or
  ``cancelled`` (exit 12). Nothing is written after a terminal message.

Install the stdout guard before importing heavy libraries so native runtimes
initialize against the redirected handles.
"""

from __future__ import annotations

import os
import sys

PROTOCOL_VERSION = 1
SUPPORTED_PROTOCOLS = (1,)
EXIT_OK = 0
EXIT_INVALID_REQUEST = 10
EXIT_UNSUPPORTED_PROTOCOL = 11
EXIT_CANCELLED = 12
EXIT_JOB_FAILED = 13
EXIT_INTERNAL_ERROR = 14


class Protocol:
    """Owns the real stdout handle; redirects every other stdout path to stderr."""

    def __init__(self) -> None:
        import threading

        sys.stdout.flush()
        self._out = os.fdopen(os.dup(1), "wb", buffering=0)  # os.dup is non-inheritable
        os.dup2(2, 1)  # CRT fd 1 -> stderr (native printf via the UCRT, os.write(1))
        if sys.platform == "win32":
            import ctypes
            import msvcrt

            kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel32.SetStdHandle.argtypes = (ctypes.c_uint32, ctypes.c_void_p)
            STD_OUTPUT_HANDLE = 0xFFFFFFF5  # (DWORD)-11
            # Win32 STD_OUTPUT_HANDLE -> stderr (GetStdHandle callers, subprocess children)
            kernel32.SetStdHandle(STD_OUTPUT_HANDLE, msvcrt.get_osfhandle(2))
        sys.stdout = sys.stderr  # Python-level print()
        self._lock = threading.Lock()
        self._seq = 0
        self._exit_code: int | None = None  # set once the terminal message is written

    def emit(self, kind: str, terminal_code: int | None = None, **fields) -> bool:
        import json

        with self._lock:
            if self._exit_code is not None:
                return False
            self._seq += 1
            message = {"protocol": PROTOCOL_VERSION, "type": kind, "seq": self._seq, **fields}
            data = (json.dumps(message, ensure_ascii=True, separators=(",", ":"), allow_nan=False) + "\n").encode("ascii")
            view = memoryview(data)
            while view:
                written = self._out.write(view)
                view = view[written:]
            if terminal_code is not None:
                self._exit_code = terminal_code
            return True

    def finish(self, kind: str, exit_code: int, **fields) -> None:
        """Emit a terminal message and exit immediately without further output.

        If another thread already sent a terminal message, exit with *its*
        code so the exit code always matches the terminal message.
        """
        self.emit(kind, terminal_code=exit_code, **fields)
        sys.stderr.flush()
        os._exit(self._exit_code)


class Cancellation:
    def __init__(self) -> None:
        import threading

        self.event = threading.Event()
        self.reason = ""

    def trigger(self, reason: str) -> None:
        if not self.event.is_set():
            self.reason = reason
            self.event.set()


def read_request_line() -> tuple[bytes | None, bytes]:
    """Read the first stdin line using raw fd reads (no BufferedReader locks)."""
    buffer = b""
    while b"\n" not in buffer:
        try:
            chunk = os.read(0, 65536)
        except OSError:
            chunk = b""
        if not chunk:
            return (buffer or None), b""
        buffer += chunk
    line, rest = buffer.split(b"\n", 1)
    return line, rest


CONTROL_POLL_SECONDS = 0.02


def stdin_peeker():
    """Return ``available() -> int | None`` (None = pipe closed), or None if not a pipe.

    A thread blocked in a synchronous ReadFile on stdin serializes other I/O on
    that file object. DLL C-runtime start-up calls GetFileType on the standard
    handles, so ``import torch`` stalls until stdin receives data (measured:
    2.5 s -> 16 s). Polling PeekNamedPipe and reading only bytes that are
    already available never leaves a read pending.
    """
    if sys.platform != "win32":
        return None
    import ctypes
    import msvcrt
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    peek = kernel32.PeekNamedPipe
    peek.argtypes = (ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p,
                     ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p)
    peek.restype = wintypes.BOOL
    handle = msvcrt.get_osfhandle(0)
    probe = wintypes.DWORD()
    if not peek(handle, None, 0, None, ctypes.byref(probe), None) and ctypes.get_last_error() != 109:
        return None  # not a pipe (console/file): fall back to blocking reads

    def available() -> int | None:
        count = wintypes.DWORD()
        if not peek(handle, None, 0, None, ctypes.byref(count), None):
            return None  # ERROR_BROKEN_PIPE: parent closed stdin
        return count.value

    return available


def control_loop(protocol: Protocol, cancel: Cancellation, pending: bytes, blocking: bool = False) -> None:
    """Background thread: parse control lines; cancel acknowledges and exits."""
    import json
    import time

    available = None if blocking else stdin_peeker()
    buffer = pending
    while True:
        while b"\n" in buffer:
            line, buffer = buffer.split(b"\n", 1)
            if not line.strip():
                continue
            try:
                message = json.loads(line)
            except ValueError:
                print(f"worker: ignoring malformed control line {line[:80]!r}", file=sys.stderr)
                continue
            if isinstance(message, dict) and message.get("type") == "cancel":
                cancel.trigger("requested")
                protocol.finish("cancelled", EXIT_CANCELLED, reason="requested")
            print(f"worker: ignoring unknown control message {message!r:.80}", file=sys.stderr)
        if available is not None:
            count = available()
            if count == 0:
                time.sleep(CONTROL_POLL_SECONDS)
                continue
            chunk = os.read(0, count) if count else b""
        else:
            try:
                chunk = os.read(0, 65536)  # blocking: only safe when no DLL loads can follow
            except OSError:
                chunk = b""
        if not chunk:
            cancel.trigger("stdin_closed")
            protocol.finish("cancelled", EXIT_CANCELLED, reason="stdin_closed")
        buffer += chunk


def validate(raw: bytes | None) -> tuple[dict | None, int, str, str]:
    import json

    if raw is None:
        return None, EXIT_INVALID_REQUEST, "invalid_request", "no request line before stdin closed"
    try:
        request = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as error:
        return None, EXIT_INVALID_REQUEST, "invalid_request", f"malformed JSON: {error}"
    if not isinstance(request, dict):
        return None, EXIT_INVALID_REQUEST, "invalid_request", "request must be a JSON object"
    version = request.get("protocol")
    if not isinstance(version, int) or isinstance(version, bool):
        return None, EXIT_INVALID_REQUEST, "invalid_request", "missing integer 'protocol'"
    if version not in SUPPORTED_PROTOCOLS:
        return None, EXIT_UNSUPPORTED_PROTOCOL, "unsupported_protocol_version", f"protocol {version} is not supported"
    if request.get("type") != "job":
        return None, EXIT_INVALID_REQUEST, "invalid_request", "'type' must be 'job'"
    for key in ("job_id", "input", "output_dir"):
        if not isinstance(request.get(key), str) or not request[key]:
            return None, EXIT_INVALID_REQUEST, "invalid_request", f"missing string '{key}'"
    return request, EXIT_OK, "", ""


def library_chatter() -> None:
    """Simulate libraries that write to every stdout path we know of."""
    import ctypes
    import warnings

    print("chatter: python print() to sys.stdout")
    warnings.warn("chatter: warnings.warn")
    os.write(1, b"chatter: os.write(1) through the CRT fd\n")
    if sys.platform == "win32":
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.GetStdHandle.restype = ctypes.c_void_p
        kernel32.WriteFile.argtypes = (ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32,
                                       ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p)
        handle = kernel32.GetStdHandle(0xFFFFFFF5)
        text = b"chatter: WriteFile(GetStdHandle(STD_OUTPUT_HANDLE))\n"
        written = ctypes.c_uint32()
        kernel32.WriteFile(handle, text, len(text), ctypes.byref(written), None)
        ucrt = ctypes.CDLL("ucrtbase")
        ucrt.puts(b"chatter: ucrtbase puts()")
        ucrt.fflush(None)
        try:
            legacy = ctypes.CDLL("msvcrt")
            legacy.printf(b"chatter: legacy msvcrt printf()\n")
            legacy.fflush(None)
        except OSError:
            pass
    try:
        from tqdm import tqdm

        for _ in tqdm(range(3), desc="chatter: tqdm"):
            pass
    except ImportError:
        pass
    print("chatter: non-ASCII stderr text Ünïcødé 曲")


def spawn_grandchild() -> int:
    import subprocess

    child = subprocess.Popen(
        [sys.executable, "-c", "import time, sys; print('grandchild alive', flush=True); time.sleep(600)"],
    )
    return child.pid


def run_job(protocol: Protocol, cancel: Cancellation, request: dict) -> None:
    import pathlib
    import time

    sim = request.get("simulate", {})
    mode = sim.get("mode", "work")
    steps = int(sim.get("steps", 10))
    step_ms = float(sim.get("step_ms", 50))
    protocol.emit("accepted", job_id=request["job_id"], pid=os.getpid(), ppid=os.getppid(),
                  python=sys.version.split()[0], executable=sys.executable)

    if sim.get("chatter"):
        library_chatter()
    if sim.get("grandchild"):
        protocol.emit("diagnostic", grandchild_pid=spawn_grandchild())

    input_path = pathlib.Path(request["input"])
    output_dir = pathlib.Path(request["output_dir"])
    protocol.emit("stage", stage="decode", index=0, count=3)
    if not input_path.is_file():
        protocol.finish("failure", EXIT_JOB_FAILED, code="input_missing", message=f"input not found: {input_path}",
                        retryable=False)

    if mode == "fail":
        protocol.finish("failure", EXIT_JOB_FAILED, code="separation_failed", message="simulated domain failure",
                        retryable=True)
    if mode == "exception":
        raise RuntimeError("simulated unexpected exception")

    protocol.emit("stage", stage="separate", index=1, count=3)
    if mode == "burst":
        for index in range(int(sim.get("messages", 10_000))):
            protocol.emit("progress", stage="separate", fraction=index / sim.get("messages", 10_000))
            if index % 1000 == 0:
                print(f"chatter during burst {index}")
                os.write(1, b"raw chatter during burst\n")
    elif mode == "torch":
        import torch

        torch.set_num_threads(max(1, os.cpu_count() // 2))
        matrix = torch.randn(1024, 1024)
        for step in range(steps):
            for _ in range(int(sim.get("matmuls", 20))):
                matrix = torch.tanh(matrix @ matrix)
            if cancel.event.is_set():
                break
            protocol.emit("progress", stage="separate", fraction=(step + 1) / steps)
    elif mode == "hang_gil":
        import ctypes

        protocol.emit("progress", stage="separate", fraction=0.0)
        # PyDLL keeps the GIL held during the call: models a native extension that
        # blocks without releasing the GIL, so the control thread cannot run.
        ctypes.PyDLL("kernel32").Sleep(int(sim.get("hang_ms", 60_000)))
    else:
        for step in range(steps):
            time.sleep(step_ms / 1000)
            if mode == "crash" and step == 2:
                import faulthandler

                # Real access violation in C (ctypes would convert it to OSError via SEH).
                faulthandler._read_null()
            if mode == "abort" and step == 2:
                os.abort()  # CRT abort(): exit code 3, like a Python fatal error
            if mode == "exit_silently" and step == 2:
                sys.stderr.flush()
                os._exit(0)
            protocol.emit("progress", stage="separate", fraction=(step + 1) / steps)

    protocol.emit("stage", stage="write", index=2, count=3)
    output_dir.mkdir(parents=True, exist_ok=True)
    output = output_dir / "drums.wav"
    output.write_bytes(b"RIFF-spike-placeholder")
    payload = "x" * (int(sim.get("big_result_kb", 0)) * 1024)
    protocol.finish(
        "result",
        EXIT_OK,
        job_id=request["job_id"],
        outputs={"drums": str(output)},
        echo={"input": str(input_path), "output_dir": str(output_dir)},
        payload=payload,
    )


def main() -> None:
    protocol = Protocol()
    cancel = Cancellation()
    if len(sys.argv) == 3 and sys.argv[1] == "--request-file":
        import pathlib

        try:
            raw, pending = pathlib.Path(sys.argv[2]).read_bytes().strip(), b""
        except OSError as error:
            protocol.finish("failure", EXIT_INVALID_REQUEST, code="invalid_request", message=str(error),
                            supported_protocols=list(SUPPORTED_PROTOCOLS))
    else:
        raw, pending = read_request_line()
    request, code, failure, message = validate(raw)
    if request is None:
        protocol.finish("failure", code, code=failure, message=message,
                        supported_protocols=list(SUPPORTED_PROTOCOLS))

    import threading

    blocking = request.get("simulate", {}).get("control") == "blocking"  # hazard reproduction only
    threading.Thread(target=control_loop, args=(protocol, cancel, pending, blocking), daemon=True).start()
    try:
        run_job(protocol, cancel, request)
    except BaseException as error:  # noqa: BLE001 - last-resort protocol failure
        import traceback

        traceback.print_exc()
        protocol.finish("failure", EXIT_INTERNAL_ERROR, code="internal_error",
                        message=f"{type(error).__name__}: {error}", retryable=False)


if __name__ == "__main__":
    main()
