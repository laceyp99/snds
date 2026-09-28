"""Tiny Windows-only CPU/memory probes via ctypes (no psutil dependency)."""

from __future__ import annotations

import ctypes
import os
import time
from ctypes import wintypes

_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_psapi = ctypes.WinDLL("psapi", use_last_error=True)


def _ft(ft: wintypes.FILETIME) -> int:
    return (ft.dwHighDateTime << 32) | ft.dwLowDateTime


def system_times() -> tuple[int, int]:
    """Return (busy, total) in 100 ns units summed over all logical CPUs."""
    idle, kernel, user = wintypes.FILETIME(), wintypes.FILETIME(), wintypes.FILETIME()
    if not _k32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kernel), ctypes.byref(user)):
        raise ctypes.WinError(ctypes.get_last_error())
    total = _ft(kernel) + _ft(user)  # kernel time includes idle time
    return total - _ft(idle), total


def cpu_load_percent(interval: float = 1.0) -> float:
    b0, t0 = system_times()
    time.sleep(interval)
    b1, t1 = system_times()
    return round(100.0 * (b1 - b0) / max(1, t1 - t0), 1)


class _PMC(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD),
        ("PageFaultCount", wintypes.DWORD),
        ("PeakWorkingSetSize", ctypes.c_size_t),
        ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t),
        ("PeakPagefileUsage", ctypes.c_size_t),
    ]


def peak_working_set_mib() -> float:
    pmc = _PMC()
    pmc.cb = ctypes.sizeof(_PMC)
    h = _k32.GetCurrentProcess()
    _psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(_PMC), wintypes.DWORD]
    if not _psapi.GetProcessMemoryInfo(h, ctypes.byref(pmc), pmc.cb):
        raise ctypes.WinError(ctypes.get_last_error())
    return round(pmc.PeakWorkingSetSize / 2**20, 1)


class LoadWindow:
    """Measure wall time plus the share of machine CPU used by *other* processes."""

    def __enter__(self) -> LoadWindow:
        self.busy0, self.total0 = system_times()
        self.proc0 = time.process_time()
        self.wall0 = time.perf_counter()
        return self

    def __exit__(self, *exc: object) -> None:
        self.seconds = time.perf_counter() - self.wall0
        busy1, total1 = system_times()
        proc = time.process_time() - self.proc0
        total = max(1, total1 - self.total0) / 1e7  # CPU-seconds available (all CPUs)
        busy = (busy1 - self.busy0) / 1e7
        self.machine_load_pct = round(100.0 * busy / total, 1)
        self.other_load_pct = round(100.0 * max(0.0, busy - proc) / total, 1)


def logical_cpus() -> int:
    return os.cpu_count() or 1
