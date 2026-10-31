# Worker transport spike

This disposable spike tests whether newline-delimited JSON (NDJSON) over
anonymous pipes is a reliable, cancellable channel between the SNDS desktop
app and its short-lived Python worker on Windows, or whether a JSON-file /
status-channel alternative is needed. It is not the Phase 2 worker.

The real parent will be the C++ JUCE app. `juce::ChildProcess` cannot write to
the child's stdin and `kill()` terminates only the direct process, so the app
is expected to use its own Win32 launcher. `winjob.py` mimics that launcher
with `ctypes` only (no pywin32):

- `CreatePipe` for stdin/stdout/stderr; parent ends are made non-inheritable
  and the child ends are passed through `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`.
- `CreateProcessW` with `CREATE_SUSPENDED | CREATE_NO_WINDOW |
  EXTENDED_STARTUPINFO_PRESENT`.
- `CreateJobObjectW` + `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`,
  `AssignProcessToJobObject` while suspended, then `ResumeThread`.
- Blocking `ReadFile` threads for stdout and stderr, `WriteFile` for stdin,
  `TerminateJobObject` for hard cancel.

## Files and versions

- `worker.py`: spike worker (stdlib only; `torch` only for the compute-cancel
  scenario).
- `winjob.py`: Win32 launcher (stdlib `ctypes`).
- `harness.py`: parent-side scenarios, assertions, and timing summary.

Run with the existing CPU environment (`uv-cpu-env` lock: CPython 3.11,
torch 2.1.2+cpu). No new environment or dependency is needed.

## Protocol shape proven here

- Request: one JSON object on the first stdin line, written with
  `ensure_ascii=True`. Required: `protocol` (int), `type: "job"`, `job_id`,
  `input`, `output_dir`. stdin stays open for the whole job.
- Control: later stdin lines; `{"protocol":1,"type":"cancel"}` cancels.
  stdin EOF also cancels (parent gone or gave up). Unknown control lines are
  logged to stderr and ignored.
- Worker messages: one `json.dumps(..., ensure_ascii=True)` line per message
  on the original stdout handle, written unbuffered under a lock. Every message
  carries `protocol` and a contiguous `seq`. Types used: `accepted`, `stage`,
  `progress`, `diagnostic`, and exactly one terminal message (`result`,
  `failure`, or `cancelled`) after which nothing is written.
- stdout guard (installed before heavy imports): `os.dup(1)` keeps a private,
  non-inheritable protocol handle; `os.dup2(2, 1)` routes CRT fd 1 to stderr;
  `SetStdHandle(STD_OUTPUT_HANDLE, stderr)` routes Win32 writers and inherited
  child handles; `sys.stdout = sys.stderr` routes Python prints. Library
  chatter, warnings, tqdm, and grandchildren therefore land on stderr.

## Reproduce

```powershell
$python = "$env:LOCALAPPDATA\snds-spikes\uv-cpu-clean\Scripts\python.exe"
& $python -X utf8 .\spikes\worker-transport\harness.py --worker-python $python --repeat 5
```

Single scenarios: `--only happy_unicode cancel_during_torch`. The harness
creates a temporary directory containing `Ünïcødé 曲 🎵` and removes it
afterwards. It exits nonzero if any scenario fails.

## Result

Passed on 2026-10-03 on Windows 11 x64 (Intel Core i5-13500H, 12 cores, 32 GB),
with the harness and the worker both on CPython 3.11.9 from
`%LOCALAPPDATA%\snds-spikes\uv-cpu-clean`. 19 scenarios × 5 runs = 95 runs,
0 failures (run 14:17:08–14:18:18 -04:00, not probed for concurrent builds).
A 1× re-verification at 18:52 -04:00, with an MSVC build running
concurrently, also passed every scenario. The latencies below are
millisecond-scale and are not CPU-throughput measurements.

| Scenario | Outcome (5/5) | Timing, median / max |
|---|---|---|
| `happy_unicode`: chatter, grandchild, non-ASCII paths | `result`, exit 0 | first message 86 / 96 ms after resume; 20 progress lines spread over ~960 ms, the first arriving ~980 ms before exit |
| `request_file`: request via `--request-file <non-ASCII path>` | `result`, exit 0 | first message 97 / 113 ms |
| `cancel_message`: cancel line while working | `cancelled`, exit 12 | ack 7.8 / 12.7 ms, exit 16.0 / 19.7 ms |
| `cancel_stdin_close`: parent closes stdin | `cancelled` (`stdin_closed`), exit 12 | ack 1.5 / 20.4 ms, exit 5.6 / 25.6 ms |
| `cancel_during_torch`: cancel during torch matmuls | `cancelled`, exit 12 | ack 4.7 / 8.2 ms, exit 71.5 / 93.0 ms |
| `hard_cancel_hung`: native call holds the GIL for 120 s | no ack within 1 s grace → `TerminateJobObject(job, 99)` | worker dead 1.5 / 2.1 ms, all 5 job processes dead 3.5 / 4.9 ms, `ActiveProcesses == 0`, stdout EOF |
| `parent_crash_job_close`: close the job handle only | tree killed | 4 or 5 processes dead in 4.2 / 16.3 ms |
| `blocking_read_hazard`: old blocking stdin reader | hazard reproduced | `import torch` stalled for the full 6 s window, then resumed 76–80 ms after any stdin write |
| `malformed_json`, `missing_field`, `empty_stdin` | `failure` `invalid_request`, exit 10 | — |
| `unsupported_protocol` (`protocol: 99`) | `failure` `unsupported_protocol_version` + `supported_protocols: [1]`, exit 11 | — |
| `job_failure` | `failure` `separation_failed`, exit 13 | — |
| `internal_exception` | `failure` `internal_error`, exit 14, traceback on stderr | — |
| `native_crash` (null read in C) | no terminal message, exit `0xC0000005` → classified crashed | — |
| `abort` (`os.abort()`) | no terminal message, exit `0xC0000409` (UCRT fast-fail, not 3) | — |
| `exit_without_result` | exit 0 without a terminal message → protocol violation | — |
| `burst_and_big_result`: 20,000 progress lines + 1 MiB result line | `result`, seq contiguous, no corrupt line | ~94k messages/s; the 1 MiB line was reassembled from ~1,000–1,600 `ReadFile` chunks |
| `direct_kill_negative_control`: no job, `TerminateProcess` on the launched process (`juce::ChildProcess::kill` equivalent) | — | grandchild survived 5/5 (orphaned) |

Further observations:

- **Blocking stdin reads stall DLL loading.** A thread blocked in a
  synchronous `ReadFile` on stdin serializes other I/O on that handle. DLL
  C-runtime start-up calls `GetFileType` on the standard handles, so
  `import torch` stalled until stdin received data. An isolated probe
  measured 2.50 s without a pending read and 15.99 s with one (it was unblocked
  only by a write after 15 s). The worker's control thread therefore polls
  `PeekNamedPipe` every 20 ms and calls `os.read` only when bytes are already
  available, so no read is ever left pending. Pipes are reliable once this is
  handled, so the file/status-channel fallback is not needed.
- **The venv `python.exe` is a launcher.** The launched PID differed from the
  interpreter PID in every run (the interpreter's parent is the launcher). With
  a plain kill of the launcher, the interpreter also exited (the launcher
  manages it), but the worker's own grandchild survived 5/5 times. The Job
  Object killed every process in all runs.
- The job contained `conhost.exe` (created by `CREATE_NO_WINDOW`) plus the
  launcher/interpreter pairs of the worker and the grandchild. Jobs are nested
  on Windows 8+, so this worked even though the harness itself may run inside
  another job.
- Chatter from `print`, `warnings`, `tqdm`, `os.write(1)`,
  `WriteFile(GetStdHandle(STD_OUTPUT_HANDLE))`, `ucrtbase puts`, and the
  grandchild appeared on stderr, never on the protocol stream. Output from the
  legacy `msvcrt.dll` `printf` was lost (that DLL's stdio was bound to the
  original handle before the guard ran) but did not corrupt the stream.
- The grandchild did not inherit the protocol handle, so stdout reached EOF as
  soon as the worker exited. It did inherit stderr, so the parent must not wait
  for stderr EOF before deciding the outcome. It closes the job first, then
  drains stderr.
- With `ensure_ascii=True` in both directions, the protocol bytes are pure
  ASCII, so they do not depend on the console code page. Paths containing
  BMP and non-BMP characters (`🎵` surrogate escapes) round-tripped
  exactly through the request, the result echo, and file creation. `-X utf8`
  is still recommended so stderr logs are UTF-8.

## Recommendation for Phase 2 `docs/protocol.md`

Use **NDJSON over anonymous pipes**, with:

1. **Request on the first stdin line** (not a request file). stdin must stay
   open anyway for cancel and parent-death detection. This keeps the command
   line fixed (`python.exe -X utf8 <worker entry>`) and avoids an extra file
   lifecycle. Both variants passed; the request file remains a fallback for
   very large requests.
2. **Win32 launcher with a Job Object** (`KILL_ON_JOB_CLOSE`, assign while
   suspended, explicit inherited-handle list, `CREATE_NO_WINDOW`). Do not use
   `juce::ChildProcess` for the worker.
3. **Cancel sequence:** write the cancel line, wait up to a 2 s grace period
   (measured acknowledgement ≤ 21 ms, exit ≤ 100 ms), then
   `TerminateJobObject(job, 99)`. Treat stdin EOF as cancel in the worker. The
   worker exits immediately after acknowledging. The parent owns the temporary
   job directory and deletes it once the tree is dead.
4. **Worker rules:** install the stdout guard before importing torch/demucs.
   Never leave a blocking read pending on stdin (use `PeekNamedPipe` polling).
   Write exactly one terminal message, and make the exit code match it.
5. **Parent rules:** drain stdout and stderr on dedicated threads for the whole
   job (pipe back-pressure blocks the worker). Require every stdout line to be a
   JSON object with `protocol` and contiguous `seq`. Treat any other line, a
   second terminal message, or an exit code that disagrees with the terminal
   message as a protocol violation. EOF without a terminal message is a crash:
   report the exit code and keep the stderr log. Close the job after exit to
   reap leftovers.

Proposed exit-code table:

| Code | Meaning | Terminal message |
|---:|---|---|
| 0 | Success | `result` |
| 10 | Request rejected (malformed JSON, schema error, missing request) | `failure` `invalid_request` |
| 11 | Unsupported protocol version | `failure` `unsupported_protocol_version` + `supported_protocols` |
| 12 | Cancelled and acknowledged (`requested` or `stdin_closed`) | `cancelled` |
| 13 | Job failed for a known reason (input, model, CUDA, separation, validation) | `failure` with a specific `code` and `retryable` |
| 14 | Unexpected internal error (traceback on stderr) | `failure` `internal_error` |
| 99 | Hard-cancelled by the parent (`TerminateJobObject` exit code) | none (parent-assigned) |
| other | Abnormal: 1/2 are Python start-up or CLI errors before the guard; `0xC0000005` access violation; `0xC0000409` abort/fast-fail; other NTSTATUS values | none → "worker crashed" |

Worker-defined codes start at 10 so they never collide with Python's own 1/2
or with CRT/NTSTATUS crash codes.

## Remaining unknowns

- The C++ launcher must be rebuilt and re-verified with this harness's
  scenarios (handle list, job assignment while suspended, `ReadFile` threads,
  command-line quoting for paths with spaces or non-ASCII characters).
- Demucs 4.0.1 `apply_model` has no progress callback, so real separation
  progress granularity (and whether the GIL-releasing torch path keeps the
  control thread responsive for full-length CPU jobs) must be confirmed in
  Phase 2. Hard cancel covers the worst case.
- Behaviour when the app itself runs inside a job that forbids nested
  assignment or breakaway (rare on Windows 11) was not tested.
- Output written by native libraries through `msvcrt.dll` stdio is lost, not
  logged. This is acceptable for diagnostics but worth knowing.
