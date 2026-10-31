# SNDS worker protocol v1 and cache schema

This document is the contract between the SNDS app (C++) and its short-lived Python worker
(`python -I -m snds_worker`), and between the worker and the on-disk song cache.

- Implementations: `worker/src/snds_worker/{protocol,jobs_protocol,schema}.py`,
  `src/model/{WorkerProtocol,SongMetadata}.{h,cpp}`.
- Transport evidence: `spikes/worker-transport/README.md`. Where this document differs from the
  spike (field naming, the stage list), this document wins; the transport mechanics are as proven.

## 1. Compatibility policy

- Every message in both directions carries an integer `protocol` and a string `type`. This
  document defines `protocol: 1`.
- **Additive changes stay in v1:** new optional fields, new event types, new failure codes, new
  suggested actions. Receivers ignore fields they do not know and log (do not fail on) events whose
  `type` they do not know. Unknown failure codes are treated as a generic failure; the `message`
  is still shown.
- **Anything else bumps `protocol`:** removing or renaming a field, changing a field's type or
  meaning, changing stage order or ids, changing exit-code meanings, or changing the terminal-event
  rules.
- **Unsupported version:** the worker answers a request whose `protocol` it does not speak with a
  `failure` (`code: "unsupported_protocol_version"`, extra field `supportedProtocols`) and exit
  code 11. The app treats an event line with an unsupported `protocol` as an explicit error: it
  stops the job and reports "worker speaks protocol N, this app speaks 1" (the app and worker are
  built from the same checkout in the alpha, so this means a stale environment).
- Fields are `camelCase` in job messages. (The older one-shot reports below use `snake_case` and
  are frozen as they are.)
- All worker stdout is NDJSON written with `ensure_ascii=True`: every non-ASCII character is a
  `\uXXXX` escape, so lines are pure ASCII and survive any code page. The app also writes its
  request as pure ASCII. Both sides decode as UTF-8 regardless.

## 2. Commands

| Command | Kind | Input | Output |
|---|---|---|---|
| `readiness` | one-shot | none | one JSON object on stdout; exit 0 ready, 1 not ready, 3 internal error |
| `fetch-model` | one-shot | none | one JSON object on stdout; exit 0 ok, 1 failed, 3 internal error |
| `prepare` | long-running job | request on the first stdin line | NDJSON events on stdout; exit codes in section 3.4 |

### 2.1 `readiness` and `fetch-model`

One-shot commands print exactly one JSON object and exit. They are documented by their
implementation (`readiness.py`, `cli.py`) and the C++ parser (`src/app/Readiness.cpp`). Changes
since the first release are additive.

- `readiness`: `{"protocol":1,"type":"readiness","ready":bool,"worker":{version,python,executable},
  "packages":{...},"compute":{...},"model":{...},"bpm_model":{...},"paths":{home,...},
  "problems":[{code,message,details?}],"elapsed_ms":n}`. `ready` is true only when `problems` is
  empty.
  - `packages` maps each pinned distribution (`numpy`, `soundfile`, `torch`, `torchaudio`,
    `demucs`, `beat-this`) to its version or `null`. Missing: `package_missing`; fails to import:
    `import_failed`.
  - `compute`: `torch_build` (`"cpu"` | `"cuda"`), `torch_cuda_version`, `cpu_available` (a small
    CPU computation worked; otherwise the problem `cpu_unavailable`), `cuda_available` (the
    driver and device are visible), `cuda_healthy` (a small kernel plus synchronize ran in a
    throwaway child process within 45 s), `cuda_device`, `cuda_check_ms`, `cuda_error` (why it is
    unavailable or unhealthy; absent when fine) and `device`. **`device` (`"cuda"` | `"cpu"`) is
    what `compute.preference: "auto"` resolves to for `prepare`: `"cuda"` only when
    `cuda_healthy`.** A CUDA failure never makes the worker "not ready"; it falls back to CPU.
  - `model` (htdemucs) and `bpm_model` (beat_this `small0`) have the same shape: `name`, `path`,
    `present`, `size_bytes`, `expected_size_bytes`, `expected_sha256`, `sha256_checked`, `sha256`
    (the measured full-file hash, when computed), `verified` (size and SHA-256 match), plus
    `signature` (`model`) or `checkpoint` (`bpm_model`). Problem codes for `model`:
    `model_missing`, `model_size_mismatch`, `model_hash_mismatch`, `model_unreadable`; for
    `bpm_model` the same with a `bpm_model_` prefix. The worker never downloads a checkpoint
    outside `fetch-model`.
  - A checkpoint is hashed in full on every call (about 0.3 s for both together).
- `fetch-model`: `{"protocol":1,"type":"fetch-model","ok":bool,"path":...,"downloaded":bool,
  "size_bytes":n,"sha256":"...","models":[{name,path,downloaded,size_bytes,sha256}],
  "problems":[...]}`. It fetches and verifies both checkpoints (`htdemucs`, then `beat_this`); a
  failure of one does not stop the other. `ok` is true only if both are installed and verified.
  `path`, `downloaded`, `size_bytes`, and `sha256` describe `htdemucs` as before; `models` lists
  every checkpoint that is installed. Checkpoints live in `SNDS_HOME\models\htdemucs\` and
  `SNDS_HOME\models\beat_this\`.
- An uncaught error prints `{"protocol":1,"type":"error","command":...,"problems":[...]}` with
  exit 3.

These commands use exit codes 0/1/2/3 (2 is an argparse usage error); the job exit codes in
section 3.4 apply only to `prepare`.

## 3. The `prepare` job

Process model: the app launches the worker with stdin, stdout, and stderr as pipes inside a Job
Object, writes one request line, and keeps stdin open for the whole job. stderr is for logs only
and is never parsed. The worker polls stdin with `PeekNamedPipe` (never a blocking read; that
stalls `import torch`) and keeps a private duplicate of the original stdout handle for events
(`EventWriter.open_stdout`), redirecting fd 1 to stderr so library chatter cannot corrupt the
stream.

### 3.1 Request (first stdin line)

```json
{
  "protocol": 1,
  "type": "prepare",
  "jobId": "20261004-a1b2c3",
  "source": {"path": "C:\\Music\\song.wav", "sizeBytes": 31457280, "sha256": "<64 lowercase hex>"},
  "jobDir": "C:\\Users\\me\\AppData\\Local\\snds\\jobs\\20261004-a1b2c3",
  "cacheDir": "C:\\Users\\me\\AppData\\Local\\snds\\cache\\<sha256>-31457280",
  "compute": {"preference": "auto"},
  "artwork": {"path": "C:\\...\\jobs\\20261004-a1b2c3\\artwork.png", "origin": "embedded"}
}
```

| Field | Rules |
|---|---|
| `jobId` | 1 to 64 characters of `A-Z a-z 0-9 _ -`, starting alphanumeric. Safe as a folder name. |
| `source.path` | Absolute Windows path (drive letter or UNC). The file is read in place and never copied or modified. |
| `source.sizeBytes`, `source.sha256` | Computed by the app; the worker re-verifies the size (and the hash while decoding) and fails with `source_changed` on a mismatch. SHA-256 is lowercase hex. |
| `jobDir` | Absolute. Temporary work directory the app created under `SNDS_HOME\jobs\<jobId>`. The worker writes everything here first. |
| `cacheDir` | Absolute. The final package location `SNDS_HOME\cache\<key>`; it must not exist yet. Same volume as `jobDir`. |
| `compute.preference` | `"auto"`: validated CUDA if the readiness check found it healthy, else CPU. `"cpu"`: force CPU (the user setting, or an explicit CPU retry after a `retry_cpu` failure). |
| `artwork` | Optional or `null`. `path` is an image the app already wrote into `jobDir` (the worker moves it into the package as `artwork.<ext>`). `origin` is `"embedded"`, `"file"` (user picked), or `"fallback"` (generated). When absent the package records `origin: "fallback"` with no file. |

Unknown extra fields are ignored. Any violation fails with `invalid_request` and exit 10 (an
unsupported `protocol` fails with exit 11 instead; the version is checked first).

Control line (later on stdin): `{"protocol":1,"type":"cancel"}`. Unknown control lines are logged
to stderr and ignored. **Closing stdin also cancels** (the app is gone or gave up).

### 3.2 Events (worker to app, one JSON object per stdout line)

Every event has `protocol` and `type`. The worker's writer also adds an integer `seq` counting
from 1 without gaps; the app may log gaps and must not rely on `seq` otherwise.

| `type` | Fields | Meaning |
|---|---|---|
| `accepted` | `jobId`, `device` (`"cuda"` or `"cpu"`) | Request validated; `device` is the compute device actually chosen. |
| `stage` | `stage`, `index` (1-based), `count` (6) | A stage begins. |
| `progress` | `stage`, `fraction` (0..1), optional `etaSeconds` | Progress inside the current stage. `etaSeconds` is present only when the worker can estimate it credibly (never a fabricated number). |
| `cancelled` | optional `reason` (`"requested"`, `"stdin_closed"`) | Terminal. The job stopped on request; incomplete audio was removed from `jobDir`. |
| `failure` | `code`, `message`, optional `details`, `retryable` (bool), optional `suggestedAction` | Terminal. |
| `result` | `cacheDir`, `summary` | Terminal. The package is published and valid. |

**Stages**, in order (`index`/`count` follow this list): `decoding` (1), `separating` (2),
`normalizing` (3), `analyzing_bpm` (4), `validating` (5), `finalizing` (6). Stage ids are part of
the contract; the app maps them to display text.

**`failure` fields.** `code` is a stable snake_case id; `message` is human-readable and safe to
show; `details` is diagnostic text for the log (for example a traceback tail); `retryable` says
whether repeating the same request could succeed; `suggestedAction` is a hint, one of `retry_cpu`,
`retry`, `free_disk_space`, `fetch_model`, `relocate_source`. Codes defined in v1:

| Code | Typical cause | Retryable / action |
|---|---|---|
| `invalid_request` | Malformed request | no |
| `unsupported_protocol_version` | Request `protocol` not supported (extra field `supportedProtocols`) | no |
| `source_unreadable` | Missing, locked, or unreadable source | `relocate_source` |
| `source_changed` | Size or SHA-256 differs from the request | no (re-fingerprint) |
| `source_unsupported` | Format or channel layout outside the supported set | no |
| `decode_failed` | Corrupt or inconsistent audio | no |
| `model_missing` | Checkpoint absent or fails verification (checked at job start) | `fetch_model` |
| `cuda_failed` | CUDA error at runtime | yes, `retry_cpu` |
| `out_of_memory` | GPU or system memory exhausted | yes, `retry_cpu` when on CUDA |
| `separation_failed` | Other separator error | no |
| `normalization_failed` | Alignment or resampling error | no |
| `bpm_failed` | Reserved and never emitted: a BPM problem of any kind (including a missing or damaged BPM checkpoint) does not fail the job; it sets `bpm.error` in `song.json` (section 5) | no |
| `validation_failed` | Output failed the stem contract | no |
| `finalize_failed` | Could not write or publish the package. `retryable: true, suggestedAction: "retry"` when the final rename keeps failing (a scanner holding the folder) | no |
| `cache_conflict` | `cacheDir` already exists and is not a valid, compatible package for this source (corrupt, incompatible, or describing another source). Nothing was changed; the app quarantines or deletes the entry and prepares again. `details` names the folder and the reason | no |
| `disk_full` | No space left | `free_disk_space` |
| `internal_error` | Unexpected exception (traceback on stderr) | no |

`result.summary` is a small object for the UI so it need not reopen `song.json`:
`{"key","durationSeconds","sampleRate","bpm","bpmConfidence","device","elapsedSeconds"}`
(additive; the authoritative data is `song.json`). `bpm` is `null` when detection failed.

### 3.3 Ordering rules

1. A request that fails validation produces one `failure` (no `accepted`) and the process exits 10
   or 11.
2. Otherwise `accepted` is the first event.
3. `stage` events appear in the fixed order, each at most once. A stage may be skipped only by
   ending the job; stages are never repeated or reordered.
4. `progress` for stage S appears only after `stage` S and before the next `stage`.
5. **Exactly one terminal event** (`result`, `failure`, or `cancelled`) is written, it is the last
   line, and nothing is written after it. The exit code must agree with it (section 3.4).
6. `result` is written only after the package has been atomically published (section 5.3).
7. Cancellation may arrive at any time; the worker acknowledges with `cancelled` (exit 12) unless a
   `result` or `failure` was already decided. A job already finishing may legitimately end with
   `result` after a cancel request; the app then uses the package.

### 3.4 Exit codes

| Code | Meaning | Terminal event |
|---|---|---|
| 0 | ok | `result` |
| 10 | bad request | `failure` (`invalid_request`) |
| 11 | unsupported protocol | `failure` (`unsupported_protocol_version`) |
| 12 | cancelled | `cancelled` |
| 13 | job failed | `failure` (any job code) |
| 14 | internal error | `failure` (`internal_error`) |
| 99 | killed by the app | none; assigned via `TerminateJobObject` after the cancel grace period |
| anything else | crash (for example `0xC0000005`, `0xC0000409`) | none |

The app decides the outcome from the exit code **and** the terminal event together
(`resolveJobOutcome`): 99 is always `killed`; a code outside the table is always `crashed`; an
exit code without its matching terminal event (including exit 0 with no `result`) is a
`protocolViolation`; the app reports it as a failed job and keeps `jobs\<jobId>` for diagnosis.
The app does not wait for stderr EOF before deciding the outcome.

Events the app cannot parse (not JSON, missing fields, unknown stage) are logged and skipped; an
event with an unsupported `protocol` ends the job as an error.

### 3.5 What each stage does

Implementation: `worker/src/snds_worker/{prepare,pipeline,decode,separation,normalize,validate}.py`.

- **Start.** The worker reads the request line (blocking, before any heavy import), starts the
  stdin control thread, resolves the device, and only then writes `accepted`. `auto` resolves to
  CUDA only if a small kernel runs on it right now (`cpu` never touches CUDA), so `accepted`
  arrives a few seconds after launch (the time to import PyTorch).
- **`decoding`.** Re-verifies `source.sizeBytes` and the SHA-256 of the whole file
  (`source_changed` on a mismatch; `source_unreadable` if it cannot be opened), then decodes it
  to float32. Accepted: WAV (`WAV`, `WAVEX`, `RF64`) with 8/16/24/32-bit PCM or 32/64-bit float
  samples, and MP3; mono or stereo; 8 kHz to 192 kHz; at most 30 minutes. The format is detected from the content,
  not the extension. Rejections: any other container, WAV sample format, channel count, or rate
  is `source_unsupported` with an actionable message; unreadable or inconsistent audio (NaN or
  infinite samples, no frames, an MP3 whose decode is shorter than its MPEG frame count) is
  `decode_failed`. The source file is only ever opened for reading.
- **`separating`.** Loads the pinned htdemucs checkpoint from `SNDS_HOME\models\htdemucs`
  after verifying its size and SHA-256 (`model_missing` + `fetch_model` otherwise; nothing is
  downloaded at job time), resamples the source to 44.1 kHz stereo (mono is duplicated), and runs
  `apply_model` segment by segment. `progress` is the share of segments done; `etaSeconds`
  appears once at least 5% and 5 s have elapsed. Cancellation is polled between segments, so on
  a slow CPU it can take up to one segment (a few seconds) to take effect. CUDA out-of-memory is
  `out_of_memory` and any other CUDA runtime error is `cuda_failed`, both `retryable: true,
  suggestedAction: "retry_cpu"`; on the CPU, memory exhaustion is `out_of_memory` with
  `retryable: false`.
- **`normalizing`.** Resamples each stem back to the source rate with exactly the source frame
  count and writes it as stereo float32 WAV to `jobDir\package\<stem>.wav`. No gain,
  normalization, or clipping; peaks above 1.0 are preserved. Differences of more than 2 frames
  from the source length, or non-finite samples, are `normalization_failed`.
- **`analyzing_bpm`.** Runs beat_this 1.1.0 (`small0` checkpoint, `SNDS_HOME\models\beat_this`,
  verified by size and SHA-256 first) on the decoded source mix, on the job's device (a CUDA
  error is retried on the CPU). The tempo is 60 divided by the mean of the beat intervals that
  are within 10% of the median interval; `confidence` is the share of intervals within 5% of
  the median; `alternatives` are the x0.5 and x2 tempos. **This stage never fails the job.** With
  fewer than three beats, a tempo outside 20 to 400 BPM, a missing or damaged checkpoint (the
  worker never downloads it), or any analysis error, the package records `bpm.value: null`,
  `confidence: 0`, `alternatives: []`, the beat count, and a short `bpm.error` (for example
  `"too few beats detected (2)"`). `bpm.analyzer` (name, version, checkpoint hash) is recorded
  either way. Only a cancel request interrupts the stage.
- **`validating`.** Reads the written files back: exactly `drums`, `bass`, `other`, `vocals`
  WAVs and nothing else; each readable, stereo, float32, at the source sample rate, with exactly
  the source frame count, only finite samples, and its peak measured. Alignment is checked on
  the sum of the stems against the source mono mix in the loudest 3 s window within the first
  and within the last 40% of the track (at most 45 s from each end; windows below -60 dBFS RMS
  are skipped with a warning, because fade-ins and fade-outs are at the noise floor): integer
  lag 0 within 50 ms, a normalized correlation of at least 0.5, and less than 0.05 samples of
  sub-sample drift between the two windows. Any violation is `validation_failed`.
- **`finalizing`.** Publishing the package (section 5.3).

Package folder: everything is written to `jobDir\package\`; `song.json` is added there last
and the folder is renamed to `cacheDir`. On `cancelled` **and** on `failure` the worker deletes
`jobDir\package` (and any `*.part` files) before writing the terminal event, so no incomplete
audio remains; other files in `jobDir` (such as the app's artwork) and the worker log
(`SNDS_HOME\logs\worker.log`) are kept. If the app closes the worker's stdout, the job stops as
`cancelled`.

Test hooks: the environment variable `SNDS_FAKE_SEPARATOR=1` replaces htdemucs with a fake that
splits the mix by fixed weights (see `fake_separator.py` for its companion variables). It exists
for automated tests; the app never sets it.

## 4. Cache layout

The package identity is the source content: `key = <sha256>-<sizeBytes>` (lowercase hex SHA-256 of
the source bytes, decimal byte size). Renaming or moving the source does not change the key;
changing its bytes does.

```
SNDS_HOME\
  cache\<sha256>-<sizeBytes>\
    song.json          written last
    drums.wav  bass.wav  other.wav  vocals.wav
    artwork.<ext>      optional (png, jpg, ...)
  jobs\<jobId>\        temporary work; the app cleans or quarantines abandoned ones
  logs\  models\
```

Stems: stereo float32 WAV at the **source** sample rate, exactly the source frame count, no gain
change or clipping (peaks above 1.0 are preserved and recorded). See
`spikes/normalized-stems/README.md`.

## 5. `song.json` (`cacheSchema: 1`)

```json
{
  "cacheSchema": 1,
  "status": "complete",
  "source": {
    "fileName": "song.wav", "path": "C:\\Music\\song.wav",
    "sizeBytes": 31457280, "sha256": "<hex>",
    "container": "wav", "sampleRate": 44100, "channels": 2,
    "frames": 441000, "durationSeconds": 10.0
  },
  "stems": {
    "drums":  {"file": "drums.wav",  "frames": 441000, "sampleRate": 44100, "channels": 2, "sampleFormat": "float32", "peak": 0.91},
    "bass":   {"file": "bass.wav",   "...": "same shape"},
    "other":  {"file": "other.wav",  "...": "same shape"},
    "vocals": {"file": "vocals.wav", "...": "same shape"}
  },
  "model": {"name": "htdemucs", "signature": "955717e8", "file": "955717e8-8726e21a.th", "sha256": "<hex>"},
  "bpm": {
    "value": 120.0, "confidence": 0.8, "beats": 20, "alternatives": [60.0, 240.0], "error": null,
    "analyzer": {"name": "beat_this", "version": "1.1.0", "checkpointSha256": "<hex>"},
    "userOverride": null
  },
  "artwork": {"file": null, "origin": "fallback"},
  "worker": {"version": "0.0.1", "protocol": 1, "torch": "2.1.2+cpu", "device": "cpu"},
  "preparedAt": "2026-10-04T12:00:00Z"
}
```

| Field | Rules |
|---|---|
| `cacheSchema` | Integer; this document is `1`. |
| `status` | Must be `"complete"` for a playable package. |
| `source.container` | `"wav"` or `"mp3"`. `path` is where the file was at preparation time and is only a hint; the app may need `Locate Original`. `fileName` is the file name of that path. |
| `source.frames`, `sampleRate`, `durationSeconds` | Positive. `channels` is the source channel count (1 or 2). |
| `stems.<name>` | All four required. `file` is exactly `<name>.wav`; `channels` is 2; `sampleFormat` is `"float32"`; `frames` equals `source.frames`; `sampleRate` equals `source.sampleRate`; `peak` is the absolute sample peak (>= 0, may exceed 1). |
| `model` | Pins the checkpoint used. |
| `bpm.value` | Detected global BPM (> 0), or `null` when detection failed or found too few beats. **A BPM failure never fails the job**: the package is still complete and the app falls back to a user-set tempo. `confidence` is 0..1 (share of beat intervals within 5% of the median; 0 when `value` is null). `beats` is the detected beat count. `alternatives` lists the x0.5 and x2 candidates (empty when `value` is null). |
| `bpm.error` | Optional. `null` (or absent) when `value` is set; otherwise a short human-readable reason (for example `"too few beats detected (3)"`). Required when `value` is null. |
| `bpm.userOverride` | `null`, or `{"value": number > 0, "mode": "numeric" \| "half" \| "double"}`. The effective BPM is the override if present, else `value`. |
| `artwork.file` | `null` or a plain file name in the package (no path separators). `origin` is `"embedded"`, `"file"`, or `"fallback"`. |
| `worker` | `protocol` is the job protocol version that produced the package; `device` is `"cuda"` or `"cpu"`. |
| `preparedAt` | UTC ISO-8601 ending in `Z`. |

Unknown additional fields are ignored (additive within schema 1).

New preparations also record `sha256` (64 lowercase hex characters) and `sizeBytes` (a positive
integer) for each stem. These optional fields must appear together. Older entries without them
remain playable. The explicit cache Verify action checks headers and finite samples, then these
hashes and sizes when available; it reports limited verification for older entries. Normal cache
listing does not scan audio content.

### 5.1 Load classification

The app and the worker's `schema.classify` apply the same rules, in this order:

1. Not JSON or not an object: **corrupt**.
2. `cacheSchema` missing or not an integer: **corrupt**. A different integer (older or newer):
   **incompatible**.
3. `status` is not `"complete"`: **corrupt**.
4. Any field above missing, mistyped, or violating its rule: **corrupt** (the reason names the
   first problem).
5. `model.name`/`model.signature` differ from the build's pinned model, or `worker.protocol`
   differs from the build's protocol: **incompatible**.

When loading from disk the app additionally requires the folder name to equal the cache key
recomputed from `source` and every stem file (and the artwork file, if named) to exist; failures
are **corrupt**. Only **complete** packages are playable.

### 5.2 Migration policy

No in-place migration in the alpha. A cache entry that is **incompatible** (older/newer schema,
different model or protocol) or **corrupt** stays listed in the cache UI with its reason, can be
deleted, and can be re-prepared (which replaces it once the new package is published). The
playable source is never required to be present for a valid package; the original is only needed
for A/B and re-preparation.

### 5.3 Finalization contract

Implementation: `worker/src/snds_worker/finalize.py`.

1. The worker writes everything (stems, artwork, then validates) under `jobs\<jobId>\package\`,
   a package directory that has **no** `song.json` yet. The app's artwork image (if any) is
   moved there as `artwork.<ext>` (a missing or odd artwork file falls back to
   `origin: "fallback"`), and every audio and image file is flushed to disk.
2. After full validation (stem names, readability, stereo float32 at the source rate, exact frame
   count, finite samples, peaks recorded), the worker builds `song.json`, validates it against
   the schema, and writes it **last**: to `song.json.tmp` in the package folder (flushed), then
   renamed to `song.json`. It then re-reads the package as the app would (schema, cache key,
   every named file present). A cancel request is honoured up to this point.
3. It then atomically renames the package directory to `cacheDir` (same volume, so a rename that
   never replaces an existing folder). A transient rename failure (Windows scanners holding the
   new folder) is retried a few times. If `cacheDir` already exists, which can only mean another
   preparation of the same source finished first:
   - and it is a **valid package for this source** (schema complete, folder name equal to the key
     recomputed from `song.json`, model and protocol compatible, every file present), the
     worker discards its own package and reports the existing folder as the `result` (the
     summary describes the existing package; the existing folder is not modified);
   - otherwise the worker fails with `cache_conflict` and changes nothing. The app deletes or
     quarantines the entry and prepares again. (`finalize_failed` is reserved for I/O
     problems.)
4. Only then does it write `result`. A cancel request after the rename cannot undo it: the job
   ends with `result` (section 3.3 rule 7).

A crash before step 3 leaves only `jobs\<jobId>\`; a cache directory without a valid
`song.json` with `status: "complete"` and a matching schema is never playable. The app quarantines
or deletes abandoned `jobs\*` directories (those with no running worker) at startup and from the
cache UI, preserving logs.

The BPM user override is the only mutation after finalization: the app rewrites `song.json` with
the new `bpm.userOverride` by writing a temporary file in the package directory and atomically
replacing `song.json`. Nothing else in the package changes.
