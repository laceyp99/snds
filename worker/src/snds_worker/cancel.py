"""Cancellation: a token the job polls, and the stdin control channel that sets it.

Transport mechanics are those proven in spikes/worker-transport: the first stdin line is the
request, later lines are control messages, and closing stdin also cancels. The worker never
leaves a blocking read pending on stdin while heavy DLLs load (that stalls `import torch`); it
polls `PeekNamedPipe` and reads only bytes that are already available.
"""

from __future__ import annotations

import logging
import os
import sys
import threading
import time
from collections.abc import Callable

from snds_worker import jobs_protocol
from snds_worker.errors import JobCancelled

log = logging.getLogger(__name__)

POLL_SECONDS = 0.02
_ERROR_BROKEN_PIPE = 109


class CancelToken:
    """Set once by the control thread; polled by every stage of the job."""

    def __init__(self) -> None:
        self._event = threading.Event()
        self._reason = "requested"
        self._lock = threading.Lock()

    def trigger(self, reason: str) -> None:
        with self._lock:
            if not self._event.is_set():
                self._reason = reason
                self._event.set()

    @property
    def cancelled(self) -> bool:
        return self._event.is_set()

    @property
    def reason(self) -> str:
        return self._reason

    def raise_if_cancelled(self) -> None:
        if self._event.is_set():
            raise JobCancelled(self._reason)

    def wait(self, seconds: float) -> bool:
        """Sleeps up to `seconds`; returns True as soon as the token is cancelled."""
        return self._event.wait(seconds)


def read_request_line() -> tuple[bytes | None, bytes]:
    """Reads the first stdin line with raw fd reads (no buffered reader).

    Returns (line or None when stdin closed before any data, bytes read after the line).
    """
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


def stdin_peeker() -> Callable[[], int | None] | None:
    """Returns `available() -> bytes ready, or None when the pipe was closed`.

    None is returned instead when stdin is not a pipe (console or file), where the caller has
    to fall back to blocking reads.
    """
    if sys.platform != "win32":
        return None

    import ctypes
    import msvcrt
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    peek = kernel32.PeekNamedPipe
    peek.argtypes = (
        ctypes.c_void_p,
        ctypes.c_void_p,
        wintypes.DWORD,
        ctypes.c_void_p,
        ctypes.POINTER(wintypes.DWORD),
        ctypes.c_void_p,
    )
    peek.restype = wintypes.BOOL

    try:
        handle = msvcrt.get_osfhandle(0)
    except OSError:
        return None

    probe = wintypes.DWORD()
    if (
        not peek(handle, None, 0, None, ctypes.byref(probe), None)
        and ctypes.get_last_error() != _ERROR_BROKEN_PIPE
    ):
        return None

    def available() -> int | None:
        count = wintypes.DWORD()
        if not peek(handle, None, 0, None, ctypes.byref(count), None):
            return None  # broken pipe: the app closed its end
        return count.value

    return available


class StdinControl:
    """Background thread that turns stdin control lines into cancellation.

    `{"type":"cancel"}` triggers reason "requested"; EOF or a broken pipe triggers
    "stdin_closed". Other lines are logged and ignored.
    """

    def __init__(self, token: CancelToken, pending: bytes = b"") -> None:
        self._token = token
        self._buffer = pending
        self._thread = threading.Thread(target=self._run, name="stdin-control", daemon=True)

    def start(self) -> None:
        self._thread.start()

    def _handle_lines(self) -> bool:
        """Processes complete lines in the buffer; returns True after a cancel."""
        while b"\n" in self._buffer:
            line, self._buffer = self._buffer.split(b"\n", 1)
            if not line.strip():
                continue
            if jobs_protocol.is_cancel_line(line):
                self._token.trigger("requested")
                return True
            log.warning("Ignoring unknown control line: %r", line[:80])
        return False

    def _run(self) -> None:
        available = stdin_peeker()
        while True:
            if self._handle_lines():
                return
            if available is not None:
                count = available()
                if count == 0:
                    time.sleep(POLL_SECONDS)
                    continue
                chunk = os.read(0, count) if count else b""
            else:
                try:
                    chunk = os.read(0, 65536)  # blocking: stdin is not a pipe
                except OSError:
                    chunk = b""
            if not chunk:
                self._token.trigger("stdin_closed")
                return
            self._buffer += chunk
