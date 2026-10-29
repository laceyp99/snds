# Worker protocol and cache contract

This is the contract between the SNDS app (C++/JUCE) and the Python preparation worker
(`worker/src/snds_worker`). It covers how the app launches and talks to the worker, every message,
the exit and error codes, the cache layout, the song-package manifest, and exactly what the app must
check before it plays a cached song.

- Protocol version: **1** (`protocol_version`)
- Manifest schema version: **1** (`schema_version`)
- Pipeline id: **`snds-p1-htdemucs-8726e21a`**

The design follows the Phase 0 evidence in [spikes.md](spikes.md): NDJSON over pipes,
cooperative cancellation, and a separate exit code for CUDA failures so the app can retry on
the CPU.

A runnable reference client lives in `snds_worker/client.py`
(`worker\.venv\Scripts\python.exe -m snds_worker.client separate <file>`). The tests in
`worker/tests/test_cli_separate.py` drive the worker exactly as described here.

## 1. Process model and transport

**One short-lived process per command.** The app starts:

```text
<checkout>\worker\.venv\Scripts\python.exe -u -X utf8 -m snds_worker <command> [options]
```

Call the interpreter directly: no shell and no venv activation. The working directory does not
matter. It is also good to set `PYTHONUNBUFFERED=1` and `PYTHONUTF8=1`.

| Command | Input | Purpose |
|---|---|---|
| `version` | `[--protocol-version N]` | Reports the worker version, supported protocol versions and pipeline id. |
| `readiness` | `[--protocol-version N] [--data-root DIR] [--force-cpu] [--full-verify] [--probe-timeout S]` | Runs the environment checks (section 4). |
| `fetch-model` | `[--protocol-version N] [--data-root DIR]` | Installs and verifies the pinned checkpoint. Idempotent. |
| `separate` | JSON request as the **first stdin line**; `[--request-timeout S]` (default 30) | Runs one preparation job (section 5). |

**Channels.**

- **stdout** carries protocol messages only. Each is one JSON object per line, UTF-8, ASCII-escaped
  (`ensure_ascii`), terminated by `\n` (never `\r\n`) and flushed per line. At startup the worker
  points file descriptor 1 at stderr, so stray library prints can never corrupt the stream.
- **stdin** is read only by `separate`. The first line is the job request. Later lines are control
  messages. **EOF on stdin means cancel.**
- **stderr** carries human-readable logs only (`2026-09-28T05:50:20.133Z INFO snds_worker.job: ...`).
  It has no fixed format. **The app must drain it continuously**; a full stderr pipe blocks the
  worker (spike scenario 9). Write it to the rotating worker log.

**What the C++ client must do** (spikes.md, "Worker transport"):

- Create the process with `CreateProcessW`, `CREATE_SUSPENDED | CREATE_NO_WINDOW`, and three
  anonymous pipes.
- Use `STARTUPINFOEX` + `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` so the child inherits only those pipe
  handles.
- Assign the process to a Job Object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, then call
  `ResumeThread`. Keep the job handle open for the worker's lifetime.
- Do not use `juce::ChildProcess`: it has no stdin and does not kill the process tree.
- Run one reader thread per output pipe:
  - stdout: split on `\n`, strip a trailing `\r` defensively, and parse one JSON object per line.
    A line that is not a JSON object with a string `type` is a protocol error: log it, and treat
    the run as abnormal if no valid terminal message follows.
  - stderr: append to the worker log.
- `separate`: write the request line and keep stdin **open** until the process exits. Closing it
  cancels the job.
- `version`, `readiness`, `fetch-model`: stdin is not read. Close it or pass `NUL`.

**Message envelope.** Every stdout message has these fields:

| Field | Type | Meaning |
|---|---|---|
| `type` | string | Message type (section 3). |
| `protocol_version` | int | Always `1` in this version. |
| `seq` | int | 1, 2, 3, ... per process. There are no gaps unless stdout broke. |
| `ts` | string | UTC ISO-8601 with milliseconds, e.g. `2026-09-28T05:59:36.257Z`. |

**Terminal message rule.** The last stdout line of every run is exactly one **terminal** message:
`result`, `failed` or `cancelled`. Its `exit_code` field equals the process exit code. The worker
never writes anything after it.

If the process exits without a terminal message, or the exit code disagrees with it, the run is
**abnormal**. Causes include a crash, a kill, an out-of-memory kill, or the Job Object being
terminated. See section 2.

## 2. Exit codes

| Code | Terminal message | Meaning | App action |
|---|---|---|---|
| 0 | `result` | Success. | For `separate`: validate the cache entry (section 8), then offer playback. |
| 1 | `failed` | The job or command failed (input, model, output, cache or internal error). | Show `error.message`, offer the action for `error.code`, keep the job log. |
| 2 | `failed` | Bad invocation or request: malformed JSON, invalid fields, unsupported protocol version, missing request, request timeout. | App bug or app/worker version skew. Do not retry the same request. |
| 3 | `failed` (`error.retry_on_cpu = true`) | CUDA unavailable or failed. | Offer an explicit CPU retry in a **new** worker process with `"device": "cpu"` and a **new** `job_id`. |
| 4 | `cancelled` | Cancelled by a cancel message, stdin EOF or Ctrl+C. | Nothing is published. |
| other | none | Abnormal: crash, hard kill, injected test crash (70), or `TerminateProcess`/`TerminateJobObject` (the exit code is whatever the killer chose). | Treat as `worker-crashed`: sweep the job directory (section 7) and show the stderr tail. |

`failed` messages carry `error.code`. The app should branch on `error.code` and use the exit code
only as a cross-check.

## 3. Messages (worker → app)

### `hello`

This is always the first line. Check `protocol_version` here. An unknown version is an
incompatible worker: do not continue, and tell the user to re-run `scripts\bootstrap.ps1` or rebuild.

```json
{"type":"hello","protocol_version":1,"seq":1,"ts":"2026-09-28T05:59:35.602Z","command":"separate","worker_version":"0.1.0","protocol_versions":[1],"pid":6724,"python":"3.12.14","pipeline_id":"snds-p1-htdemucs-8726e21a"}
```

### `stage`

A stage has started. `stage_index` is 0-based and `stage_count` is fixed per command. `label` is
English UI text; localize by `stage` if needed.

```json
{"type":"stage","protocol_version":1,"seq":2,"ts":"...","stage":"validating-input","stage_index":0,"stage_count":7,"label":"Checking the source file"}
```

The `separate` stages always come in this order, and any stage can be the last one before a
terminal message:

| # | `stage` | Work | Overall weight |
|---|---|---|---|
| 0 | `validating-input` | stat, size check, SHA-256 re-verification, format checks, decode | 0.05 |
| 1 | `loading-model` | device selection (CUDA probe), hash-verified model load, resample to 44.1 kHz | 0.03 |
| 2 | `separating` | htdemucs, segment by segment | 0.75 |
| 3 | `normalizing` | resample each stem back to the source rate, exact trim/pad, write float WAV | 0.07 |
| 4 | `analyzing-bpm` | global BPM (section 6) | 0.03 |
| 5 | `validating-output` | re-open the stems and check the contract; hash and fsync them | 0.06 |
| 6 | `finalizing` | source-unchanged check, manifest, atomic publication | 0.01 |

The `fetch-model` stages are `checking-model`, then `downloading` (only when needed), then
`installing`. `readiness` and `version` have no stages.

### `progress`

```json
{"type":"progress","protocol_version":1,"seq":15,"ts":"...","stage":"separating","stage_progress":0.2222,"overall":0.2467,"detail":{"segments_done":2,"segments_total":9}}
```

- `stage_progress` and `overall` are in `[0, 1]`.
- `overall` never decreases within a run. The stage weights are rough CPU proportions, so it is
  not a time estimate.
- Messages are throttled: one is sent after 0.25 s or a 5 % step, whichever comes first. The 0 %
  and 100 % updates of every stage are always sent. On a fast GPU, several segments can be
  folded into one message.
- `detail` is optional and varies by stage:
  - `bytes_hashed`
  - `segments_done` and `segments_total`
  - `stems_written`
  - `bytes` and `bytes_total` (download)
- A separation segment takes about 2.4 s on a 4-core CPU and about 0.2 s on an RTX 3060.

### `device`

Emitted once in `loading-model` with the compute decision:

```json
{"type":"device","protocol_version":1,"seq":10,"ts":"...","requested":"auto","selected":"cuda:0","reason":null,"cuda_available":true,"device_name":"NVIDIA GeForce RTX 3060","torch_version":"2.11.0+cu128","torch_cuda":"12.8","probe_error":null,"threads":4}
```

`reason` explains why the CPU was used:

- `forced-cpu`
- `cuda-not-available`
- `cuda-probe-failed` (also sends a `cuda-fallback` warning)
- `test-separator`
- `null` when CUDA was selected

### `log`

`level` is `info` or `warning`. Warnings carry a stable `code` (table below) and an optional
`detail` object. Show warnings to the user where useful. Errors are never `log` messages; they are
`failed` messages.

```json
{"type":"log","protocol_version":1,"seq":30,"ts":"...","level":"warning","message":"Tempo 87.0 BPM has low confidence (0.21).","code":"bpm-low-confidence"}
```

| Warning code | Meaning |
|---|---|
| `cuda-fallback` | The CUDA health probe failed; the job runs on the CPU. |
| `residual-high` | The stem sum differs from the source by more than −20 dB in-band. The entry is still valid. |
| `subsample-lag` | The sub-sample lag between the stem sum and the source is more than 0.05 samples. The entry is still valid. |
| `mp3-no-gapless-info` | The MP3 has no LAME/Xing tag. Decoders may disagree by about 1105 samples. See `source.mp3` in the manifest. |
| `bpm-low-confidence` | The BPM confidence is below 0.3. Suggest a half/double or numeric correction. |
| `bpm-unavailable` | No tempo was detected (too short or silent). The app falls back to its default BPM. |
| `replaced-cache-entry` | An invalid or incompatible entry for this key was replaced. |
| `cleanup-incomplete` | Some unpublished files could not be deleted. The app's job sweep removes them. |
| `control-message-ignored` | A stdin control line was not understood and was ignored. |
| `model-replaced` | `fetch-model` found an invalid checkpoint and downloaded it again. |

### `cancel_ack`

The worker received a cancel request. It is sent from the stdin thread immediately, before the job
stops. `reason` is `cancel-message` or `stdin-eof`. `stage` is the stage that was running.

```json
{"type":"cancel_ack","protocol_version":1,"seq":16,"ts":"...","reason":"cancel-message","stage":"separating"}
```

A `cancel_ack` is **not** the outcome. The terminal message decides the outcome (section 7).

### `cancelled` (terminal, exit 4)

```json
{"type":"cancelled","protocol_version":1,"seq":17,"ts":"...","command":"separate","reason":"cancel-message","stage":"separating","exit_code":4,"job_dir":"C:\\Users\\me\\AppData\\Local\\snds\\jobs\\demo-job-2","incomplete_audio_removed":true}
```

### `failed` (terminal, exit 1/2/3)

```json
{"type":"failed","protocol_version":1,"seq":21,"ts":"...","command":"separate","stage":"separating","error":{"code":"cuda-failed","message":"CUDA failed during separation: CUDA error: unspecified launch failure","detail":{"exception":"AcceleratorError"},"retry_on_cpu":true},"exit_code":3}
```

`stage` is `null` when the failure happened before any stage started, for example a request error.

### `result` (terminal, exit 0)

The payload depends on `command`:

- `version`: `worker_version`, `protocol_versions`, `pipeline_id`, `python`.
- `readiness`: `readiness` object (section 4).
- `fetch-model`: `model` object with `path`, `size`, `sha256`, `downloaded`, `yaml_written`, `url`
  and `http`.
- `separate`:

```json
{"type":"result","protocol_version":1,"seq":42,"ts":"...","command":"separate","exit_code":0,
 "job_id":"demo-job-1","cache_key":"7535...a3f5-2390444",
 "cache_dir":"C:\\...\\snds\\cache\\7535...a3f5-2390444","manifest_path":"C:\\...\\manifest.json",
 "publish":"published","replaced_state":null,"pipeline_id":"snds-p1-htdemucs-8726e21a","device":"cuda:0",
 "bpm":{"status":"ok","value":120.19,"confidence":0.843,"half":{"bpm":60.09,"relative_salience":0.93},"double":null,"signal":"drums"},
 "validation_status":"passed","warnings":[],"timings":{"validating-input":0.008,"loading-model":6.997,"separating":1.169,"normalizing":0.086,"analyzing-bpm":0.058,"validating-output":0.19,"finalizing":0.02}}
```

`publish` has one of these values:

- `published`: a new entry.
- `reused-existing`: an equal, fully valid entry already existed and was kept. The new output was
  discarded.
- `replaced-existing`: an invalid, incompatible or unknown-schema entry was replaced. The old state
  is in `replaced_state`.

**The result is a hint. The cache entry is the truth.** Always validate the entry (section 8)
before playback.

### Control messages (app → worker, stdin, `separate` only)

| Line | Effect |
|---|---|
| `{"type":"cancel","protocol_version":1}` | Cancel. `protocol_version` is optional here. |
| stdin EOF | Cancel with reason `stdin-eof`. EOF also arrives when the app dies. |
| anything else | Ignored, with a `control-message-ignored` warning. |

Compatibility rules for the app:

- Ignore unknown fields in worker messages. Within protocol version 1 the worker may add fields.
- Log and ignore unknown message types.
- The worker rejects unknown fields in *requests*.

## 4. `readiness`

`python -m snds_worker readiness --protocol-version 1 --data-root <root> [--force-cpu]`

The worker exits 0 whenever it produced a report, even if checks are `missing`. Statuses match
`snds::ReadinessStatus`:

- `ok`
- `warning`: usable, but worth attention
- `missing`: blocks separation until the user acts

```json
{"type":"result","command":"readiness","exit_code":0,"readiness":{
  "separation_available":true,
  "compute":{"requested":"auto","selected":"cuda:0","reason":null,"device_name":"NVIDIA GeForce RTX 3060"},
  "pipeline_id":"snds-p1-htdemucs-8726e21a",
  "model":{"name":"htdemucs","signature":"955717e8","checkpoint_file":"955717e8-8726e21a.th","checkpoint_sha256":"8726e21a...","checkpoint_size":84141911,"url":"https://dl.fbaipublicfiles.com/...","samplerate":44100,"channels":2,"sources":["drums","bass","other","vocals"]},
  "data_root":"C:\\Users\\me\\AppData\\Local\\snds",
  "checks":[{"id":"model-checkpoint","title":"Separation model","status":"ok","detail":"...(SHA-256 verified via memo)","action":"","data":{"present":true,"size":84141911,"sha256":"8726e21a...","verified_via":"memo","ok":true,"problem":null,...}}, ...]}}
```

Every check has the fields `id`, `title`, `status`, `detail`, `action` (empty when nothing is
needed) and `data` (machine-readable). The worker check ids never collide with the app's local ids
(`worker-project`, `uv`, `worker-environment`, `model-directory`):

| `id` | Checks | `missing` when |
|---|---|---|
| `worker-python` | Python version (pinned 3.12) | never (`warning` if not 3.12) |
| `worker-packages` | numpy/scipy/soundfile/torch/demucs installed and importable; version drift from the lock is a `warning`; `data.torch_build` = `cpu`/`cuda` | something is missing or does not import |
| `mp3-decoder` | libsndfile has MP3 support | MP3 is unsupported |
| `data-directories` | Data root is writable; `cache/` and `jobs/` are on the same volume; free space (`warning` below 2 GiB) | not writable, or on different volumes |
| `model-checkpoint` | Presence, size and SHA-256 of the pinned checkpoint | absent or not matching the pin |
| `cpu` | CPU name, logical processors, torch threads (`warning` below 4 threads) | never |
| `cuda` | CUDA build, device, capability and memory; a real 256×256 matmul compared with the CPU (bounded by `--probe-timeout`, default 30 s); skipped with `--force-cpu` | never (`warning` when the probe fails and the CPU will be used) |
| `worker-test-mode` | Present only when `SNDS_WORKER_TEST_*` variables are set | never (`warning`) |

**Model hash memo.** The full SHA-256 of the 84 MB file takes about 0.1 s warm. The worker still
records `models/htdemucs/.snds-verified.json`, which holds the size, the `mtime_ns` and the SHA-256
of the last full verification. `readiness` reports `verified_via: "memo"` while the size and mtime
are unchanged, and re-hashes otherwise or with `--full-verify`. A separation job ignores the memo:
it hashes the exact bytes it loads.

`compute` shows what `auto` would select right now. The job repeats the probe itself, because
devices can change between readiness and a job.

## 5. `separate` job request (app → worker, first stdin line)

```json
{"type":"separate","protocol_version":1,"job_id":"20260928T055935Z-3f2a9c",
 "source":{"path":"D:\\Music\\song.mp3","sha256":"<64 lowercase hex>","size_bytes":7340032},
 "options":{"device":"auto"},
 "data_root":"C:\\Users\\me\\AppData\\Local\\snds"}
```

| Field | Required | Rules |
|---|---|---|
| `type` | yes | `"separate"` |
| `protocol_version` | yes | Integer in `[1]`. It is checked **before** anything else. Any other value, including `"1"`, `true`, `1.0` or a missing field, gives `unsupported-protocol-version` (exit 2). |
| `job_id` | yes | `[A-Za-z0-9][A-Za-z0-9_-]{0,63}`, new for every run, **including a CPU retry**. It names `jobs/<job_id>/`. |
| `source.path` | yes | Absolute path to the original WAV or MP3, which is never modified or copied. |
| `source.sha256` | yes | SHA-256 of the file bytes, computed by the app. |
| `source.size_bytes` | yes | The file size in bytes. |
| `options.device` | no | `auto` (default: validated CUDA, else CPU), `cpu` (the Force CPU setting and the explicit retry), or `cuda` (CUDA or exit 3). |
| `data_root` | no | Absolute path. It defaults to the same resolution as `AppPaths`: an absolute `SNDS_DATA_DIR`, else `%LOCALAPPDATA%\snds`. **The app should always pass its `AppPaths` root.** |

Unknown fields at any level give `invalid-request`. The request line must be at most 1 MiB. A
missing request (EOF) gives `request-missing`; no request within `--request-timeout` gives
`request-timeout`.

**Fingerprint policy.** The app owns hashing and computes the cache key. The worker **re-verifies**
the fingerprint:

1. `stat` the file. The size must equal `size_bytes`.
2. Stream a full SHA-256, which must equal `sha256`. Hashing costs about one sequential read, well
   under a second for typical songs, and is small next to separation.
3. Decode.
4. Re-`stat` after decoding and again just before publishing. The size and `mtime_ns` must be
   unchanged.

A mismatch in step 1 or 2 is `source-fingerprint-mismatch`. A change in step 4 is `source-changed`.
This guarantees that stems are never cached under the key of different audio.

**Input contract.** Accepted inputs:

- **Containers and subtypes:**
  - WAV / WAVEX / RF64 with `PCM_U8`, `PCM_S8`, `PCM_16`, `PCM_24`, `PCM_32`, `FLOAT` or `DOUBLE`.
  - MP3 (MPEG layer III).
- **Channels:** mono or stereo.
- **Sample rate:** 8–192 kHz.
- **Duration:** 1 frame to 30 minutes. The limit keeps every float32 stem under the 4 GiB WAV limit
  and bounds memory.
- **Samples:** finite only.

MP3s are decoded by libsndfile + mpg123, which is gapless with a LAME/Xing tag. **The decoded frame
count is authoritative**, not the header estimate.

## 6. Processing pipeline (pipeline `snds-p1-htdemucs-8726e21a`)

Stem data depends on everything below, so a change to any of it requires a new pipeline id.

1. **Decode** to float32 `(frames, channels)`. Mono is duplicated to stereo.
2. **Resample** to 44.1 kHz with `scipy.signal.resample_poly`. It uses the exact reduced ratio,
   Kaiser(5.0), is zero-phase, and produces `ceil(n·up/down)` frames.
3. **Standardize** globally, as `demucs.api` does: `(x − mean) / (std + 1e-8)`.
4. **Separate** with `apply_model(shifts=0, split=True, overlap=0.25)`, using the model's 7.8 s
   segments. The mix stays on the CPU and only segments go to the GPU.
5. **Restore** the level: `stem·std + mean/4`, so the stems still sum to the mix.
6. **Normalize** each stem: resample 44.1 kHz → source rate, then trim or zero-pad to **exactly**
   the decoded source frame count.
7. **Write** each stem as a stereo **IEEE float32 WAV** at the source rate. Stems are never clipped,
   limited or normalized, and peaks are recorded. Headroom belongs to the app's master stage.
   Stems at 44.1 kHz use about 85 MB per minute of audio.
8. **Analyze BPM** on 44.1 kHz mono signals with the in-house estimator:
   1. Build a spectral-flux onset envelope.
   2. Take its autocorrelation.
   3. Score comb salience times a log-normal prior centred on 120 BPM.
   4. Refine using long lags.
   The range is **40–240 BPM**. Clips shorter than 5 s give `insufficient-audio`; silence gives
   `no-onsets`.

   **Signal choice.** The worker analyzes the separated `drums` stem first, because it has the pulse
   without vocal, bass and pad note changes that create spurious onsets. It always analyzes the
   full mix too. It uses the mix when either of these is true:

   - The drums stem is more than 30 dB below the mix (no drums).
   - The drums confidence is more than 0.1 below the mix confidence.

   Both candidates are stored. Octave errors (for example 70 read as 140, or 174 as 87) are
   expected. The chosen `half`/`double` alternatives carry `relative_salience` (near 1.0 means
   "equally plausible") so the correction UI can offer them.
9. **Validate the output.** This is the agreed tolerance, and every item is a hard failure unless
   marked otherwise:
   - All 4 stems exist and each is a WAV / `FLOAT` / 2-channel file at the source sample rate.
   - `frames == decoded source frames`, **exactly**.
   - All samples are finite.
   - The integer cross-correlation lag between the stem sum and the source is **0**. This holds in
     each of 5 two-second windows spread over the file (silent windows are skipped) and in their
     aggregate. The search covers ±2048 samples, which catches the 1105-sample MP3 priming offset.
     The lag is positive when the stems are late.
   - The in-band residual (stem sum − source) is **≤ −10 dB**. Above −20 dB gives a `residual-high`
     warning. Measured values are about −33 dB. The band is ≤ min(20 kHz, 0.45·fs) and is measured
     with a Hann STFT, because htdemucs cannot reproduce content above 22.05 kHz.
   - Sub-sample lag above 0.05 samples gives a `subsample-lag` warning only.
10. **Finalize atomically** (section 7).

## 7. Cache layout, atomicity and cancellation

```text
<data root>\                         %LOCALAPPDATA%\snds unless SNDS_DATA_DIR is absolute
  cache\<cache_key>\                 one validated song package
      manifest.json                  written last; schema below
      stems\drums.wav                stereo float32 WAV at the source rate, exact source length
      stems\bass.wav
      stems\other.wav
      stems\vocals.wav
      overrides.json                 optional, owned by the app (user BPM correction)
  jobs\<job_id>\                     worker scratch space (same volume as cache\)
      status.json                    job state, rewritten atomically at every stage
      job.log                        this job's worker log
      package\                       the future cache entry (stems\, manifest.json)
      replaced-<n>\                  an old entry moved aside during replacement (transient)
  models\htdemucs\955717e8-8726e21a.th, htdemucs.yaml, .snds-verified.json
  settings\, logs\                   app-owned
```

**Cache key:** `cache_key = <source sha256, 64 lowercase hex> + "-" + <source size in bytes, decimal>`.
It matches `^[0-9a-f]{64}-(0|[1-9][0-9]*)$`. Directory names in `cache\` that do not match are not
entries. The cache UI should report them as foreign or quarantined and never play them.

**Atomicity guarantees.**

1. All output goes to `jobs\<job_id>\package\`. `cache\` and `jobs\` must be on the same volume;
   the worker checks this and reports `data-dir-unavailable` otherwise.
2. The stems are written, then re-opened and validated. They are hashed and `fsync`ed
   (FlushFileBuffers) in `validating-output`.
3. `manifest.json` is written **last**: to a temp file, then `fsync`, then `os.replace`. A package
   without a manifest is incomplete by definition.
4. Publication is **one directory rename**, `package` → `cache\<key>`, using `MoveFileExW` without
   `REPLACE_EXISTING`. This rename is the **point of no return**; a cancel that arrives after it
   is too late, and the job still ends with `result`.
   - **No entry yet:** the rename publishes it. The rename fails if another publisher got there
     first. That never overwrites; the worker then re-inspects.
   - **An entry that passes the deep check** (section 8, including stem SHA-256): kept, ours is
     discarded (`reused-existing`).
   - **An invalid, incompatible or unknown-schema entry** is replaced:
     1. Copy its `overrides.json` into our package.
     2. Rename the old entry to `jobs\<job_id>\replaced-<n>`.
     3. Rename ours into place.
     4. Delete the old entry.

     A crash between steps 2 and 3 leaves **no** entry, never a partial one. If the old entry cannot
     be moved because the app holds files open in it, the job fails with `cache-publish-failed`.
     The app must not keep files of an entry open while re-preparing that entry.
5. On success the job directory is deleted. On failure or cancel the worker deletes `package\` and
   `replaced-*`, and keeps `status.json` and `job.log`.

So `cache\<key>\` either does not exist or is a complete, validated package. A crash or kill at any
point never produces a partial entry. The app's check in section 8 is still required: it is what
protects against disk corruption, manual edits and old or new schemas.

**`status.json`** (`schema_version` 1, `kind` `snds-job-status`) has these fields:

- `job_id`, `pid`, `created_at`, `updated_at`
- `state`: `running`, `publishing`, `published`, `failed` or `cancelled`
- `stage`, `cache_key`, `source_path`, `exit_code`
- `error` (`{code, message}`) or `reason` (for a cancel)

**Abandoned jobs.** The app runs one job at a time, so every `jobs\<id>\` other than the live job's
is abandoned. That covers crashed or killed workers (state `running` or `publishing`, possibly with
`package\`) and finished failures or cancels. Recommended sweep, at startup and after an abnormal
exit:

1. Delete `package\` and `replaced-*` immediately. They are never playable.
2. Keep `status.json` and `job.log` for diagnostics.
3. Remove the whole directory after a few days, or on "clear cache".

An abandoned job never blocks a new job for the same song.

**Cancellation semantics.**

- **Triggers:**
  - A `cancel` line.
  - stdin EOF, including the app dying.
  - Ctrl+C in a console (`reason: "interrupt"`).
- **Acknowledgement:** `cancel_ack` is emitted immediately.
- **Cancellation points:**
  - stage boundaries
  - every 1 MiB of source hashing
  - the start and end of every separation segment (the demucs 4.1.0 callback raises and aborts the
    remaining segments)
  - between stems in `normalizing` and `validating-output`
  - just before publication
- **Latency:** at most one segment, about 2.4 s on CPU and 0.2 s on CUDA. Decoding and model
  loading are not interruptible; each takes about 1 s at most, except the first CUDA
  initialisation, which takes a few seconds.
- **Outcome:** incomplete audio is deleted, `status.json` gets `state: "cancelled"`, `job.log` is
  kept, the worker emits `cancelled` and exits 4.
- **App sequence** (spikes.md):
  1. Write `{"type":"cancel"}\n` and close stdin.
  2. Wait up to about 5 s for `cancelled` and the process exit.
  3. If it does not finish, call `TerminateJobObject` and treat the run as abnormal.
  4. Remove `package\` from the job directory, as for any abandoned job.

**CUDA failure mid-job.** A runtime or out-of-memory error on CUDA during separation, including
moving the model to the GPU, gives `failed` with `cuda-failed` and `retry_on_cpu: true`, and exit 3. The app must
retry in a **new process**: a poisoned CUDA context is never reused. The retry uses
`"device": "cpu"` and a new `job_id`.

## 8. Song-package manifest (`manifest.json`, `schema_version` 1)

```jsonc
{
  "schema_version": 1,
  "kind": "snds-song-package",
  "cache_key": "<sha256>-<size>",
  "created_at": "2026-09-28T05:50:30.043Z",
  "pipeline": {
    "pipeline_id": "snds-p1-htdemucs-8726e21a",     // compatibility identity (see below)
    "separator": "htdemucs",
    "model": {"name": "htdemucs", "signature": "955717e8", "checkpoint_file": "955717e8-8726e21a.th",
              "checkpoint_sha256": "8726e21a…", "checkpoint_size": 84141911, "url": "…",
              "samplerate": 44100, "channels": 2, "sources": ["drums","bass","other","vocals"]},
    "separation": {"shifts": 0, "split": true, "overlap": 0.25, "input_standardization": true, "segment_seconds": 7.8},
    "resampler": {"name": "scipy.signal.resample_poly", "window": ["kaiser", 5.0], "zero_phase": true},
    "stem_format": {"container": "WAV", "subtype": "FLOAT", "channels": 2},
    "test_mode": null                                // non-null only for test-seam output
  },
  "producer": {"worker_version": "0.1.0", "protocol_version": 1, "python": "3.12.14",
               "packages": {"numpy": "2.5.3", "scipy": "1.18.1", "soundfile": "0.14.0",
                            "torch": "2.11.0+cu128", "demucs": "4.1.0", "libsndfile": "1.2.2"}},
  "job": {"job_id": "…", "started_at": "…", "finished_at": "…", "timings": {"validating-input": 0.008, "…": 0}},
  "compute": {"requested": "auto", "selected": "cuda:0", "reason": null, "cuda_available": true,
              "device_name": "NVIDIA GeForce RTX 3060", "torch_version": "2.11.0+cu128", "torch_cuda": "12.8",
              "probe_error": null, "threads": 4},
  "source": {
    "fingerprint": {"sha256": "…", "size_bytes": 2390444},
    "path_at_preparation": "D:\\Music\\song.mp3",    // local only; redact in diagnostics
    "file_name": "song.mp3",
    "format": "MP3", "subtype": "MPEG_LAYER_III",
    "samplerate": 48000, "channels": 2,
    "header_frames": 398400,                         // container estimate (may differ for MP3)
    "decoded_frames": 398400,                        // authoritative; the stems have exactly this many
    "duration_seconds": 8.3,
    "decoder": {"name": "libsndfile", "version": "1.2.2"},
    "mp3": {"id3v2_bytes": 0, "mpeg_samplerate": 48000, "samples_per_frame": 1152, "audio_frames": 348,
            "raw_frames": 400896, "info_tag": {"tag": "Info", "encoder": "LAME3.100", "encoder_delay": 576,
            "padding": 1920}, "gapless_info": true, "gapless_frames": 398400},   // null for WAV
    "peak_dbfs": -3.0
  },
  "stems": {
    "order": ["drums", "bass", "other", "vocals"],
    "samplerate": 48000, "channels": 2, "frames": 398400, "subtype": "FLOAT",
    "files": {"drums": {"path": "stems/drums.wav", "bytes": 3187288, "sha256": "…", "peak_dbfs": -5.71}, "…": {}},
    "sum_peak_dbfs": -3.78,
    "samples_over_0dbfs": 0                          // informational: stems may exceed 0 dBFS
  },
  "validation": {
    "status": "passed",                              // or "passed-with-warnings"
    "tolerance": {"frames": "exact", "lag_samples": 0, "subsample_warn": 0.05, "residual_fail_db": -10.0, "residual_warn_db": -20.0},
    "failures": [],
    "warnings": [],                                  // [{code, message, …}]
    "metrics": {"stems_checked": 4, "samples_over_0dbfs": 0, "stem_sum_peak_dbfs": -3.78, "source_peak_dbfs": -3.0,
                "alignment": {"applicable": true, "lag_samples": 0, "lag_subsample": 0.0002, "max_lag_searched": 2048,
                              "window_seconds": 2.0, "windows": [{"start_frame": 0, "lag": 0}, "…"]},
                "residual_db_inband": -33.24, "residual_db_fullband": -20.62, "residual_band_hz": 20000.0}
  },
  "bpm": {
    "status": "ok",                                  // "ok" | "insufficient-audio" | "no-onsets"
    "value": 120.19, "confidence": 0.843,            // null when status != ok
    "half": {"bpm": 60.09, "relative_salience": 0.93}, "double": null,   // null when outside the range
    "signal": "drums",                               // "drums" | "mix" | null
    "range": [40.0, 240.0],
    "estimator": {"name": "snds-acf-tempo", "version": 1},
    "drums_relative_level_db": -5.59,
    "candidates": {"mix": {"status": "ok", "bpm": 120.19, "confidence": 0.843, "half": {}, "double": null},
                   "drums": {"status": "ok", "bpm": 120.19, "confidence": 0.843, "half": {}, "double": null}}
  },
  "warnings": [{"code": "…", "message": "…"}]       // every warning emitted during the job
}
```

Peak values are dBFS (`null` for digital silence). Paths inside the manifest are relative to the
entry directory and use `/`.

### What the app must validate before playback

The Python reference is `snds_worker.cache.inspect_entry`. An entry is **playable** only if every
check passes:

1. `cache\<key>\` is a directory whose name is the key the app computed from the source fingerprint
   (or from the recent-files record when the source is missing).
2. `manifest.json` exists, is at most 4 MiB, and parses as a JSON object. Otherwise the entry is
   *invalid*.
3. `schema_version`:
   - An integer other than 1 that is > 1 → **`unknown-schema`**. Treat it as incompatible and never
     modify it silently.
   - Missing, non-integer, or < 1 → *invalid*.
4. `kind == "snds-song-package"` and `cache_key` equals the directory name.
5. `source.fingerprint.sha256 + "-" + source.fingerprint.size_bytes == cache_key`.
6. `validation.status` is `passed` or `passed-with-warnings`.
7. `stems.order == ["drums","bass","other","vocals"]`, `stems.channels == 2`,
   `stems.samplerate == source.samplerate` and `stems.frames == source.decoded_frames`.
8. For each stem, `stems.files[<name>].path == "stems/<name>.wav"`, and the file:
   - exists;
   - has a byte size equal to `bytes`;
   - has a WAV header that reads as IEEE float 32-bit, 2 channels, `stems.samplerate` and
     `stems.frames` frames. JUCE `WavAudioFormat` reader: `usesFloatingPointData`,
     `bitsPerSample == 32`, `numChannels == 2`, `sampleRate`, `lengthInSamples`.
9. `pipeline.pipeline_id` equals the app's expected id, `snds-p1-htdemucs-8726e21a`. Hard-code it so
   cached songs play even when the worker is unavailable. `readiness`/`version` report the worker's
   id; if the two differ, show a setup warning. A different id means **incompatible** (stale
   provenance): offer re-preparation, which replaces the entry.
10. **Deep check** (optional, recommended after a crash, on a "verify cache" action, or when a
    reader error occurs): the SHA-256 of every stem equals `files[<name>].sha256`.

Resulting states:

- `valid`: playable.
- `missing`: no entry.
- `invalid`: corrupt or incomplete. Offer re-preparation, or quarantine/delete.
- `incompatible`: a different pipeline. Offer re-preparation.
- `unknown-schema`: prepared by a newer SNDS. Offer re-preparation.

Only `valid` is playable. **Never start playback from a job directory, a directory without a
manifest, or an entry in any state other than `valid`.**

**Missing source flow.** The cache entry does not depend on the original file. When the original
is missing or moved, the entry stays playable (Stem Mix only). Original A/B needs the original,
re-verified by fingerprint.

## 9. User BPM correction (`overrides.json`, app-owned)

The worker never rewrites validated artifacts, so user corrections live in a separate small file
next to the manifest:

```json
{"schema_version": 1, "bpm": {"value": 87.0, "source": "user-half", "updated_at": "2026-09-28T06:00:00.000Z"}}
```

- `bpm.source` is one of `user-numeric`, `user-half` or `user-double`.
- `bpm.value` must be a finite number in 20–300. The app enforces this; detection itself stays
  within 40–240.
- The app writes the file **atomically**: a temp file in the same directory, flush, then
  `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`.
- **Effective BPM** is taken from the first of these that applies:
  1. `overrides.bpm.value`, if the file parses, has `schema_version == 1` and the value is in range;
  2. `manifest.bpm.value`, if not null;
  3. the app's default, 120.
- A broken or unknown `overrides.json` is ignored with a warning. It **never** invalidates the
  entry.
- "Reset to detected" deletes the file.
- When the worker replaces an invalid or incompatible entry, it carries `overrides.json` over,
  because the audio identity (the cache key) is the same.

## 10. Error codes

Error codes are stable, kebab-case, and in `error.code` of `failed`.

| Code | Exit | Stage | Meaning / suggested action |
|---|---|---|---|
| `bad-invocation` | 2 | – | Invalid command line (app bug or version skew). |
| `malformed-request` | 2 | – | The request is not UTF-8 JSON, not an object, or larger than 1 MiB. |
| `request-missing` | 2 | – | stdin closed before the request. |
| `request-timeout` | 2 | – | No request within `--request-timeout`. |
| `unsupported-protocol-version` | 2 | – | `detail: {requested, supported}`. Worker and app are out of sync: re-run bootstrap or rebuild. |
| `invalid-request` | 2 | – | A field is missing, of the wrong type, invalid or unknown. `detail` names the fields. |
| `dependency-missing` | 1 | loading-model | torch/demucs are not importable. Run `scripts\bootstrap.ps1`. |
| `model-missing` | 1 | loading-model | The checkpoint is absent. Run `scripts\bootstrap.ps1`. |
| `model-corrupt` | 1 | loading-model | The checkpoint size or SHA-256 differs from the pin. Run bootstrap (it downloads again). |
| `model-load-failed` | 1 | loading-model | The checkpoint does not load, or has unexpected sources or rate. |
| `model-download-failed` | 1 | downloading | `fetch-model` could not download a matching file (network or pin mismatch); retry later. |
| `data-dir-unavailable` | 1 | – | The data root is not creatable or writable, or `cache\`/`jobs\` are on different volumes. |
| `job-exists` | 1 | – | `jobs\<job_id>` already exists; use a new `job_id`. |
| `source-not-found` | 1 | validating-input | The original is gone. Offer *Locate Original*. |
| `source-unreadable` | 1 | validating-input | Permission or I/O error, or not a regular file. |
| `source-fingerprint-mismatch` | 1 | validating-input | The size or SHA-256 differs from the request. Re-fingerprint and re-run. |
| `source-changed` | 1 | validating-input / finalizing | The file was modified during preparation. |
| `unsupported-format` | 1 | validating-input | Not WAV/MP3, or an unsupported WAV subtype (ADPCM, µ-law, …). |
| `unsupported-channel-count` | 1 | validating-input | More than 2 channels. |
| `unsupported-sample-rate` | 1 | validating-input | Outside 8–192 kHz. |
| `source-empty` | 1 | validating-input | No audio frames. |
| `source-too-long` | 1 | validating-input | Longer than 30 minutes. |
| `decode-failed` | 1 | validating-input | The decoder error, or NaN/Inf samples in a float WAV. |
| `cuda-unavailable` | 3 | loading-model | `device: "cuda"` was requested but CUDA is absent or its probe failed. Retry on CPU. |
| `cuda-failed` | 3 | separating | A CUDA runtime or out-of-memory error. Retry on CPU in a new process. |
| `separation-failed` | 1 | separating | A model error on the CPU. |
| `out-of-memory` | 1 | any | CPU memory exhausted. Close other apps or use a shorter file. |
| `disk-full` | 1 | normalizing / finalizing | No space for the stems (about 85 MB/min at 44.1 kHz). |
| `output-write-failed` | 1 | normalizing / finalizing | Another write error. |
| `output-validation-failed` | 1 | validating-output | `detail.failures[]` has sub-codes: `missing-stem`, `unreadable-stem`, `wrong-format`, `wrong-channel-count`, `wrong-sample-rate`, `wrong-frame-count`, `non-finite-samples`, `misaligned`, `residual-too-high`; `detail.metrics` has the measurements. |
| `cache-publish-failed` | 1 | finalizing | The existing entry could not be replaced (files open) or there was a persistent race. Retry. |
| `internal-error` | 1 | any | A bug. Attach `job.log` and stderr to diagnostics. |

## 11. Compatibility and migration policy (`unknown-cache-schema`)

**Protocol.**

- Any incompatible change to requests, message meanings, stages or exit codes bumps
  `protocol_version`.
- Additive fields in worker messages do not bump it. The app ignores unknown fields.
- The worker rejects requests with an unknown `protocol_version` (exit 2).
- The app rejects a worker whose `hello.protocol_version` it does not know.
- App and worker ship in the same checkout, so skew only happens mid-update. The fix is
  `scripts\bootstrap.ps1` plus a rebuild.

**Manifest schema.**

- `schema_version` changes when a field is removed, renamed or re-typed, or its meaning changes.
- New optional fields may be added within a version. Readers ignore unknown fields.
- Entries with a newer `schema_version` are `unknown-schema`: never modified automatically, and
  replaced only by an explicit re-preparation or deleted by the user.
- Older versions (none exist yet) will be migrated on read only if a documented migration exists;
  otherwise they are `incompatible`.

**Pipeline id.**

- `pipeline.pipeline_id` changes whenever stem bytes could change: the checkpoint, separation
  parameters, input standardization, resampler, stem format or normalization.
- Worker releases that do not change output keep the id, so existing caches stay valid.
- Entries with another id are `incompatible` and re-preparable.
- The fake test separator uses `snds-test-fake-separator`, which production code must never accept.

**Job status and overrides** have their own `schema_version` (1). Unknown versions are ignored:
`status.json` is diagnostics only, and an unknown `overrides.json` is treated as absent.

## 12. Test seams (for Python and C++ integration tests)

These are inert unless set. `readiness` reports them as a `worker-test-mode` warning, and manifests
record them in `pipeline.test_mode`.

| Variable | Effect |
|---|---|
| `SNDS_WORKER_TEST_SEPARATOR=fake` | A torch-free separator: four scaled copies (0.4/0.3/0.2/0.1) of the mix in 1 s segments. It exercises the real protocol, normalization, validation, BPM and publication without the model. |
| `SNDS_WORKER_TEST_SEGMENT_DELAY=<s>` | Sleep per fake segment, which makes cancellation timing deterministic. |
| `SNDS_WORKER_TEST_FAULT=<f>[,<f>…]` | Inject failures. See the list below. |

Values for `SNDS_WORKER_TEST_FAULT`:

- `device-failure@N` → `cuda-failed`, exit 3, after N segments. With the real model it triggers
  only on CUDA.
- `cuda-probe` → the probe fails, and `auto` falls back to the CPU.
- `nan-output`, `drop-frame`, `delete-stem` → `output-validation-failed`.
- `crash-before-publish`, `crash-after-aside` → a hard exit with code 70 and no terminal message.
- `internal-error` → exit 1.

For example, the C++ worker-client tests can run the real worker with
`SNDS_WORKER_TEST_SEPARATOR=fake` and a temporary `data_root`. A 10 s job finishes in about 1 s.

The Python tests are run with `worker\.venv\Scripts\python.exe -m pytest` from `worker\`.
Tests marked `model` and `cuda` run the real checkpoint and GPU, and are skipped when those are
absent.
