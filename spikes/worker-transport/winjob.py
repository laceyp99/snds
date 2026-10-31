"""Minimal ctypes Win32 launcher mirroring the planned C++ worker launcher.

* Anonymous pipes for stdin/stdout/stderr; only the child ends are
  inheritable and they are passed through PROC_THREAD_ATTRIBUTE_HANDLE_LIST
  so concurrently launched children never inherit each other's pipes.
* CreateProcessW(CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT).
* A Job Object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE; the process is assigned
  while suspended, so it cannot spawn anything outside the job, then resumed.
* TerminateJobObject kills the whole tree; closing the last job handle (for
  example when the app crashes) does the same.

No pywin32; standard library only.
"""

from __future__ import annotations

import ctypes
import dataclasses
import queue
import subprocess
import threading
import time
from ctypes import wintypes as wt

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

HANDLE = ctypes.c_void_p
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
HANDLE_FLAG_INHERIT = 0x1
STARTF_USESTDHANDLES = 0x100
CREATE_SUSPENDED = 0x4
CREATE_UNICODE_ENVIRONMENT = 0x400
EXTENDED_STARTUPINFO_PRESENT = 0x80000
CREATE_NO_WINDOW = 0x08000000
PROC_THREAD_ATTRIBUTE_HANDLE_LIST = 0x20002
JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000
JobObjectBasicAccountingInformation = 1
JobObjectBasicProcessIdList = 3
JobObjectExtendedLimitInformation = 9
WAIT_OBJECT_0 = 0
SYNCHRONIZE = 0x00100000
PROCESS_TERMINATE = 0x0001
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
STILL_ACTIVE = 259
ERROR_BROKEN_PIPE = 109


class SECURITY_ATTRIBUTES(ctypes.Structure):
    _fields_ = [("nLength", wt.DWORD), ("lpSecurityDescriptor", wt.LPVOID), ("bInheritHandle", wt.BOOL)]


class STARTUPINFOW(ctypes.Structure):
    _fields_ = [
        ("cb", wt.DWORD), ("lpReserved", wt.LPWSTR), ("lpDesktop", wt.LPWSTR), ("lpTitle", wt.LPWSTR),
        ("dwX", wt.DWORD), ("dwY", wt.DWORD), ("dwXSize", wt.DWORD), ("dwYSize", wt.DWORD),
        ("dwXCountChars", wt.DWORD), ("dwYCountChars", wt.DWORD), ("dwFillAttribute", wt.DWORD),
        ("dwFlags", wt.DWORD), ("wShowWindow", wt.WORD), ("cbReserved2", wt.WORD),
        ("lpReserved2", ctypes.c_void_p), ("hStdInput", HANDLE), ("hStdOutput", HANDLE), ("hStdError", HANDLE),
    ]


class STARTUPINFOEXW(ctypes.Structure):
    _fields_ = [("StartupInfo", STARTUPINFOW), ("lpAttributeList", ctypes.c_void_p)]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("hProcess", HANDLE), ("hThread", HANDLE), ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD)]


class JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("PerProcessUserTimeLimit", ctypes.c_int64), ("PerJobUserTimeLimit", ctypes.c_int64),
        ("LimitFlags", wt.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
        ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wt.DWORD),
        ("Affinity", ctypes.c_size_t), ("PriorityClass", wt.DWORD), ("SchedulingClass", wt.DWORD),
    ]


class IO_COUNTERS(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint64) for name in (
        "ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
        "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]


class JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("BasicLimitInformation", JOBOBJECT_BASIC_LIMIT_INFORMATION), ("IoInfo", IO_COUNTERS),
        ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
        ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t),
    ]


class JOBOBJECT_BASIC_ACCOUNTING_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("TotalUserTime", ctypes.c_int64), ("TotalKernelTime", ctypes.c_int64),
        ("ThisPeriodTotalUserTime", ctypes.c_int64), ("ThisPeriodTotalKernelTime", ctypes.c_int64),
        ("TotalPageFaultCount", wt.DWORD), ("TotalProcesses", wt.DWORD),
        ("ActiveProcesses", wt.DWORD), ("TotalTerminatedProcesses", wt.DWORD),
    ]


def _errcheck(result, func, args):
    if not result:
        raise ctypes.WinError(ctypes.get_last_error())
    return result


def _proto(name, restype, *argtypes, check=True):
    function = getattr(kernel32, name)
    function.restype = restype
    function.argtypes = argtypes
    if check:
        function.errcheck = _errcheck
    return function


CreatePipe = _proto("CreatePipe", wt.BOOL, ctypes.POINTER(HANDLE), ctypes.POINTER(HANDLE),
                    ctypes.POINTER(SECURITY_ATTRIBUTES), wt.DWORD)
SetHandleInformation = _proto("SetHandleInformation", wt.BOOL, HANDLE, wt.DWORD, wt.DWORD)
InitializeProcThreadAttributeList = _proto("InitializeProcThreadAttributeList", wt.BOOL, ctypes.c_void_p,
                                           wt.DWORD, wt.DWORD, ctypes.POINTER(ctypes.c_size_t), check=False)
UpdateProcThreadAttribute = _proto("UpdateProcThreadAttribute", wt.BOOL, ctypes.c_void_p, wt.DWORD,
                                   ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p)
DeleteProcThreadAttributeList = _proto("DeleteProcThreadAttributeList", None, ctypes.c_void_p, check=False)
CreateProcessW = _proto("CreateProcessW", wt.BOOL, wt.LPCWSTR, wt.LPWSTR, ctypes.c_void_p, ctypes.c_void_p,
                        wt.BOOL, wt.DWORD, ctypes.c_void_p, wt.LPCWSTR, ctypes.c_void_p,
                        ctypes.POINTER(PROCESS_INFORMATION))
CreateJobObjectW = _proto("CreateJobObjectW", HANDLE, ctypes.c_void_p, wt.LPCWSTR)
SetInformationJobObject = _proto("SetInformationJobObject", wt.BOOL, HANDLE, ctypes.c_int, ctypes.c_void_p, wt.DWORD)
QueryInformationJobObject = _proto("QueryInformationJobObject", wt.BOOL, HANDLE, ctypes.c_int, ctypes.c_void_p,
                                   wt.DWORD, ctypes.POINTER(wt.DWORD))
AssignProcessToJobObject = _proto("AssignProcessToJobObject", wt.BOOL, HANDLE, HANDLE)
TerminateJobObject = _proto("TerminateJobObject", wt.BOOL, HANDLE, wt.UINT)
ResumeThread = _proto("ResumeThread", wt.DWORD, HANDLE, check=False)
ReadFile = _proto("ReadFile", wt.BOOL, HANDLE, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD),
                  ctypes.c_void_p, check=False)
WriteFile = _proto("WriteFile", wt.BOOL, HANDLE, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD),
                   ctypes.c_void_p, check=False)
CloseHandle = _proto("CloseHandle", wt.BOOL, HANDLE, check=False)
WaitForSingleObject = _proto("WaitForSingleObject", wt.DWORD, HANDLE, wt.DWORD, check=False)
GetExitCodeProcess = _proto("GetExitCodeProcess", wt.BOOL, HANDLE, ctypes.POINTER(wt.DWORD))
OpenProcess = _proto("OpenProcess", HANDLE, wt.DWORD, wt.BOOL, wt.DWORD, check=False)
TerminateProcess = _proto("TerminateProcess", wt.BOOL, HANDLE, wt.UINT, check=False)


def _pipe(sa: SECURITY_ATTRIBUTES, parent_reads: bool) -> tuple[int, int]:
    """Return (parent_end, child_end); only the child end stays inheritable."""
    read, write = HANDLE(), HANDLE()
    CreatePipe(ctypes.byref(read), ctypes.byref(write), ctypes.byref(sa), 0)
    parent, child = (read.value, write.value) if parent_reads else (write.value, read.value)
    SetHandleInformation(parent, HANDLE_FLAG_INHERIT, 0)
    return parent, child


class PipeReader(threading.Thread):
    """Blocking ReadFile loop that splits lines and timestamps their arrival."""

    def __init__(self, handle: int, name: str) -> None:
        super().__init__(daemon=True, name=name)
        self.handle = handle
        self.lines: queue.Queue[tuple[float, bytes]] = queue.Queue()
        self.raw = bytearray()
        self.eof_at: float | None = None
        self.read_calls = 0

    def run(self) -> None:
        buffer = ctypes.create_string_buffer(65536)
        count = wt.DWORD()
        pending = b""
        while True:
            ok = ReadFile(self.handle, buffer, len(buffer), ctypes.byref(count), None)
            if not ok or count.value == 0:  # ERROR_BROKEN_PIPE: every writer closed
                break
            self.read_calls += 1
            chunk = buffer.raw[: count.value]
            self.raw += chunk
            pending += chunk
            now = time.perf_counter()
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                self.lines.put((now, line))
        if pending:
            self.lines.put((time.perf_counter(), pending))  # unterminated tail
        self.eof_at = time.perf_counter()
        CloseHandle(self.handle)


@dataclasses.dataclass
class Child:
    pid: int
    process: int
    job: int | None
    stdin: int | None
    stdout: PipeReader
    stderr: PipeReader
    launched_at: float

    def write(self, data: bytes) -> bool:
        if self.stdin is None:
            return False
        written = wt.DWORD()
        view = memoryview(data)
        while view:
            chunk = bytes(view[:65536])
            if not WriteFile(self.stdin, chunk, len(chunk), ctypes.byref(written), None):
                return False  # ERROR_NO_DATA / ERROR_BROKEN_PIPE: reader is gone
            view = view[written.value:]
        return True

    def close_stdin(self) -> None:
        if self.stdin is not None:
            CloseHandle(self.stdin)
            self.stdin = None

    def wait(self, timeout: float) -> bool:
        return WaitForSingleObject(self.process, int(timeout * 1000)) == WAIT_OBJECT_0

    def exit_code(self) -> int:
        code = wt.DWORD()
        GetExitCodeProcess(self.process, ctypes.byref(code))
        return code.value

    def terminate_job(self, code: int) -> None:
        TerminateJobObject(self.job, code)

    def close_job(self) -> None:
        if self.job is not None:
            CloseHandle(self.job)
            self.job = None

    def job_pids(self) -> list[int]:
        return job_pids(self.job) if self.job else []

    def job_active(self) -> int:
        info = JOBOBJECT_BASIC_ACCOUNTING_INFORMATION()
        QueryInformationJobObject(self.job, JobObjectBasicAccountingInformation, ctypes.byref(info),
                                  ctypes.sizeof(info), None)
        return info.ActiveProcesses

    def close(self) -> None:
        self.close_stdin()
        self.close_job()
        CloseHandle(self.process)


def job_pids(job: int) -> list[int]:
    class Pids(ctypes.Structure):
        _fields_ = [("Assigned", wt.DWORD), ("InList", wt.DWORD), ("Ids", ctypes.c_size_t * 256)]

    pids = Pids()
    QueryInformationJobObject(job, JobObjectBasicProcessIdList, ctypes.byref(pids), ctypes.sizeof(pids), None)
    return [int(pids.Ids[i]) for i in range(pids.InList)]


def open_process(pid: int, terminate: bool = False) -> int | None:
    access = SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | (PROCESS_TERMINATE if terminate else 0)
    return OpenProcess(access, False, pid) or None


QueryFullProcessImageNameW = _proto("QueryFullProcessImageNameW", wt.BOOL, HANDLE, wt.DWORD, wt.LPWSTR,
                                   ctypes.POINTER(wt.DWORD), check=False)


def process_image(pid: int) -> str:
    handle = open_process(pid)
    if not handle:
        return "?"
    try:
        size = wt.DWORD(1024)
        buffer = ctypes.create_unicode_buffer(size.value)
        if not QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):
            return "?"
        return buffer.value
    finally:
        CloseHandle(handle)


def process_alive(handle: int) -> bool:
    return WaitForSingleObject(handle, 0) != WAIT_OBJECT_0


def launch(argv: list[str], use_job: bool = True, cwd: str | None = None) -> Child:
    sa = SECURITY_ATTRIBUTES(ctypes.sizeof(SECURITY_ATTRIBUTES), None, True)
    stdin_parent, stdin_child = _pipe(sa, parent_reads=False)
    stdout_parent, stdout_child = _pipe(sa, parent_reads=True)
    stderr_parent, stderr_child = _pipe(sa, parent_reads=True)

    size = ctypes.c_size_t()
    InitializeProcThreadAttributeList(None, 1, 0, ctypes.byref(size))
    attributes = ctypes.create_string_buffer(size.value)
    if not InitializeProcThreadAttributeList(attributes, 1, 0, ctypes.byref(size)):
        raise ctypes.WinError(ctypes.get_last_error())
    inherit = (HANDLE * 3)(stdin_child, stdout_child, stderr_child)
    UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                              ctypes.sizeof(inherit), None, None)

    startup = STARTUPINFOEXW()
    startup.StartupInfo.cb = ctypes.sizeof(STARTUPINFOEXW)
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES
    startup.StartupInfo.hStdInput = stdin_child
    startup.StartupInfo.hStdOutput = stdout_child
    startup.StartupInfo.hStdError = stderr_child
    startup.lpAttributeList = ctypes.cast(attributes, ctypes.c_void_p)

    info = PROCESS_INFORMATION()
    command = ctypes.create_unicode_buffer(subprocess.list2cmdline(argv))
    flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT
    try:
        CreateProcessW(None, command, None, None, True, flags, None, cwd, ctypes.byref(startup), ctypes.byref(info))
    finally:
        DeleteProcThreadAttributeList(attributes)
        for handle in (stdin_child, stdout_child, stderr_child):
            CloseHandle(handle)  # parent must drop child ends or EOF never arrives

    job = None
    if use_job:
        job = CreateJobObjectW(None, None)
        limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, ctypes.byref(limits), ctypes.sizeof(limits))
        AssignProcessToJobObject(job, info.hProcess)
    launched_at = time.perf_counter()
    ResumeThread(info.hThread)
    CloseHandle(info.hThread)

    stdout = PipeReader(stdout_parent, "stdout")
    stderr = PipeReader(stderr_parent, "stderr")
    stdout.start()
    stderr.start()
    return Child(info.dwProcessId, info.hProcess, job, stdin_parent, stdout, stderr, launched_at)
