"""stdin control channel: the first line is the job request, later lines are control messages.

``{"type": "cancel"}`` or stdin EOF (the app closed the pipe or died) requests cancellation.
stdin is read with ``os.read`` on the raw descriptor, not ``sys.stdin``: a daemon thread
blocked inside a buffered ``sys.stdin`` read can abort interpreter shutdown on Windows.
"""

from __future__ import annotations

import json
import logging
import os
import queue
import threading
from collections.abc import Callable

from snds_worker.protocol import MAX_LINE_BYTES, JobCancelled

log = logging.getLogger(__name__)

CANCEL_MESSAGE = "cancel-message"
CANCEL_STDIN_EOF = "stdin-eof"
CANCEL_INTERRUPT = "interrupt"


class CancelToken:
    def __init__(self) -> None:
        self._event = threading.Event()
        self._lock = threading.Lock()
        self.reason: str | None = None

    def request(self, reason: str) -> bool:
        """Returns True for the first request only."""
        with self._lock:
            if self._event.is_set():
                return False
            self.reason = reason
            self._event.set()
            return True

    @property
    def requested(self) -> bool:
        return self._event.is_set()

    def check(self) -> None:
        if self._event.is_set():
            raise JobCancelled(self.reason or CANCEL_MESSAGE)


class _Eof:
    pass


class _TooLong:
    pass


EOF = _Eof()
TOO_LONG = _TooLong()


class StdinChannel:
    """Reader thread over fd 0. ``wait_request()`` returns the first line (bytes), ``EOF`` or
    ``TOO_LONG``. Afterwards control lines drive ``token`` and ``on_cancel(reason)``;
    ``on_ignored(text)`` reports control lines that were not understood."""

    def __init__(
        self,
        token: CancelToken,
        on_cancel: Callable[[str], None],
        on_ignored: Callable[[str], None] | None = None,
        fd: int = 0,
    ) -> None:
        self.token = token
        self.on_cancel = on_cancel
        self.on_ignored = on_ignored
        self.fd = fd
        self._requests: queue.Queue = queue.Queue(maxsize=1)
        self._have_request = False
        self._thread = threading.Thread(target=self._run, name="stdin-control", daemon=True)

    def start(self) -> None:
        self._thread.start()

    def wait_request(self, timeout: float | None) -> bytes | _Eof | _TooLong | None:
        """None on timeout."""
        try:
            return self._requests.get(timeout=timeout)
        except queue.Empty:
            return None

    def _cancel(self, reason: str) -> None:
        if self.token.request(reason):
            try:
                self.on_cancel(reason)
            except Exception:  # never let the reader thread die on a callback error
                log.exception("cancel callback failed")

    def _line(self, line: bytes) -> None:
        if not self._have_request:
            self._have_request = True
            self._requests.put(line)
            return
        text = line.decode("utf-8", "replace").strip()
        if not text:
            return
        try:
            msg = json.loads(text)
        except json.JSONDecodeError:
            msg = None
        if isinstance(msg, dict) and msg.get("type") == "cancel":
            self._cancel(CANCEL_MESSAGE)
        elif self.on_ignored is not None:
            self.on_ignored(text[:200])

    def _run(self) -> None:
        buf = b""
        discarding = False
        while True:
            try:
                chunk = os.read(self.fd, 65536)
            except OSError:
                chunk = b""
            if not chunk:
                break
            buf += chunk
            while True:
                nl = buf.find(b"\n")
                if nl < 0:
                    break
                line, buf = buf[:nl], buf[nl + 1 :]
                if discarding:
                    discarding = False
                    continue
                self._line(line.rstrip(b"\r"))
            if len(buf) > MAX_LINE_BYTES and not discarding:
                if not self._have_request:
                    self._have_request = True
                    self._requests.put(TOO_LONG)
                discarding = True
                buf = b""
            elif discarding:
                buf = b""
        # EOF: the app closed stdin (or exited).
        if not self._have_request:
            self._have_request = True
            if buf.strip() and not discarding:
                self._requests.put(buf.rstrip(b"\r"))  # request without a trailing newline
            else:
                self._requests.put(EOF)
                return
        self._cancel(CANCEL_STDIN_EOF)
