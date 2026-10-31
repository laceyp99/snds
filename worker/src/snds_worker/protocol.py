"""Worker output protocol: each command writes exactly one JSON object to stdout.

Everything else (logging, progress, library chatter) goes to stderr, so the app can parse
stdout without filtering.
"""

from __future__ import annotations

import contextlib
import json
import os
import sys
from collections.abc import Callable, Iterator
from typing import Any

PROTOCOL_VERSION = 1


def message(message_type: str, **fields: Any) -> dict[str, Any]:
    return {"protocol": PROTOCOL_VERSION, "type": message_type, **fields}


def problem(code: str, text: str, details: str | None = None) -> dict[str, str]:
    entry = {"code": code, "message": text}
    if details:
        entry["details"] = details
    return entry


def ensure_standard_streams() -> None:
    """Gives the process a usable stdin and stderr if the launcher passed none.

    juce::ChildProcess starts the worker without stdin or stderr handles when it reads only
    stdout. Python then has sys.stderr = None and no file descriptor 2, which would break
    logging and stdout_to_stderr(). Both are pointed at the null device instead.
    """
    for fd, name, flags in ((0, "stdin", os.O_RDONLY), (2, "stderr", os.O_WRONLY)):
        try:
            os.fstat(fd)
        except OSError:
            null_fd = os.open(os.devnull, flags)
            if null_fd != fd:
                os.dup2(null_fd, fd)
                os.close(null_fd)
        if getattr(sys, name) is None:
            mode = "r" if fd == 0 else "w"
            setattr(sys, name, open(fd, mode, encoding="utf-8", closefd=False))  # noqa: SIM115


def emit(payload: dict[str, Any]) -> None:
    sys.stdout.write(json.dumps(payload, ensure_ascii=True) + "\n")
    sys.stdout.flush()


@contextlib.contextmanager
def stdout_to_stderr() -> Iterator[None]:
    """Routes stdout to stderr for the duration, at both the Python and the file-descriptor
    level, so that stray prints from native libraries cannot corrupt the JSON on stdout."""
    sys.stdout.flush()
    try:
        saved_fd = os.dup(1)
    except OSError:  # no usable stdout descriptor (for example under some test runners)
        saved_fd = None

    restore_handle = _redirect_win32_stdout_handle() if saved_fd is not None else None
    try:
        if saved_fd is not None:
            os.dup2(2, 1)
        with contextlib.redirect_stdout(sys.stderr):
            yield
    finally:
        sys.stdout.flush()
        if saved_fd is not None:
            os.dup2(saved_fd, 1)
            os.close(saved_fd)
        if restore_handle is not None:
            restore_handle()


def _redirect_win32_stdout_handle() -> Callable[[], None] | None:
    """Points the Win32 STD_OUTPUT_HANDLE at stderr, for native code that calls GetStdHandle
    (and for child processes that inherit it). Returns a function that undoes it."""
    if sys.platform != "win32":
        return None
    try:
        import ctypes

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.GetStdHandle.restype = ctypes.c_void_p
        kernel32.GetStdHandle.argtypes = (ctypes.c_uint32,)
        kernel32.SetStdHandle.argtypes = (ctypes.c_uint32, ctypes.c_void_p)
        stdout_id, stderr_id = 0xFFFFFFF5, 0xFFFFFFF4  # (DWORD)-11 and (DWORD)-12
        original = kernel32.GetStdHandle(stdout_id)
        kernel32.SetStdHandle(stdout_id, kernel32.GetStdHandle(stderr_id))
    except (OSError, AttributeError):
        return None
    return lambda: kernel32.SetStdHandle(stdout_id, original)
