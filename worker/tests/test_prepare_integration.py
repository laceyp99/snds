"""Subprocess integration tests for `prepare` with the fake separator on tiny fixtures.

These run the worker exactly as the app does (`python -I -m snds_worker prepare`, pipes, one
request line) and cover what the unit tests cannot: real stdin cancellation at every stage,
abnormal exit, request framing, and the published package on disk. Synthetic audio is generated
into temp directories; nothing here needs the model or the network.
"""

from __future__ import annotations

import json
import sys
import time
from pathlib import Path

import pytest
import soundfile as sf

from helpers import synth_audio, write_mp3, write_wav
from procs import check_stream, request_for_source
from snds_worker import finalize, jobs_protocol, schema

STEMS = ("drums", "bass", "other", "vocals")

# Runs the real worker, but holds the job for a moment right after a chosen `stage` event, so
# that a cancel line sent when that event is seen always arrives while the stage is current.
HOLD_AT_STAGE = """
import os, time
from snds_worker import jobs_protocol
real_emit = jobs_protocol.EventWriter.emit
def emit(self, event):
    written = real_emit(self, event)
    if event.get("type") == "stage" and event.get("stage") == os.environ["SNDS_TEST_HOLD_STAGE"]:
        time.sleep(float(os.environ.get("SNDS_TEST_HOLD_SECONDS", "1.5")))
    return written
jobs_protocol.EventWriter.emit = emit
from snds_worker.cli import main
raise SystemExit(main(["prepare"]))
"""


def cache_entries(tmp_path: Path) -> list[Path]:
    root = tmp_path / "cache"
    return sorted(root.iterdir()) if root.exists() else []


# --- Fixtures: WAV and MP3, mono and stereo ----------------------------------------------------


@pytest.mark.parametrize(
    ("kind", "rate", "channels"),
    [
        ("wav", 44_100, 2),
        ("wav", 48_000, 1),
        ("wav", 96_000, 2),
        ("mp3", 44_100, 2),
        ("mp3", 44_100, 1),
    ],
)
def test_prepare_publishes_wav_and_mp3(
    tmp_path: Path, started, kind: str, rate: int, channels: int
) -> None:
    write = write_wav if kind == "wav" else write_mp3
    source = write(tmp_path / f"song.{kind}", rate, 6.0, channels)
    request = request_for_source(source, tmp_path)
    job = started()

    job.send(request)
    events, code = job.finish()

    check_stream(job.raw, events)
    assert code == jobs_protocol.EXIT_OK, events[-1]
    assert events[-1]["type"] == "result"
    cache = Path(request["cacheDir"])
    assert cache_entries(tmp_path) == [cache]
    assert finalize.verify_package(cache, cache.name)[0] == schema.COMPLETE

    data = json.loads((cache / "song.json").read_text(encoding="utf-8"))
    frames = sf.info(str(source)).frames
    assert data["source"]["container"] == kind
    assert data["source"]["channels"] == channels
    assert data["source"]["sampleRate"] == rate
    assert data["source"]["frames"] == frames
    for name in STEMS:
        info = sf.info(str(cache / f"{name}.wav"))
        assert (info.frames, info.samplerate, info.channels, info.subtype) == (
            frames,
            rate,
            2,
            "FLOAT",
        )
    assert data["bpm"]["analyzer"]["name"] == "beat_this"
    assert data["bpm"]["value"] is None  # no BPM checkpoint in this SNDS_HOME: recorded, not fatal
    assert "missing or damaged" in data["bpm"]["error"]
    assert events[-1]["summary"]["bpm"] is None


def test_every_message_type_is_seen_on_the_wire(tmp_path: Path, started) -> None:
    seen: set[str] = set()

    ok = started()
    ok.send(request_for_source(write_wav(tmp_path / "a.wav", 44_100, 3.0, seed=1), tmp_path))
    events, _ = ok.finish()
    seen |= {e["type"] for e in events}

    bad = started({"SNDS_FAKE_FAIL": "error"})
    bad.send(request_for_source(write_wav(tmp_path / "b.wav", 44_100, 3.0, seed=2), tmp_path))
    events, _ = bad.finish()
    seen |= {e["type"] for e in events}

    cancelled = started({"SNDS_FAKE_SEGMENTS": "100", "SNDS_FAKE_SEGMENT_MS": "50"})
    cancelled.send(request_for_source(write_wav(tmp_path / "c.wav", 44_100, 3.0, seed=3), tmp_path))
    cancelled.events_until(lambda e: e["type"] == "progress")
    cancelled.send({"protocol": 1, "type": "cancel"})
    events, _ = cancelled.finish()
    seen |= {e["type"] for e in events}

    assert seen == {"accepted", "stage", "progress", "result", "failure", "cancelled"}


# --- Cancellation at every stage -------------------------------------------------------------


@pytest.mark.parametrize("stage", list(jobs_protocol.STAGES))
def test_cancel_at_every_stage_over_stdin(tmp_path: Path, home: Path, started, stage: str) -> None:
    source = write_wav(tmp_path / "song.wav", 44_100, 4.0)
    request = request_for_source(source, tmp_path)
    artwork = Path(request["jobDir"]) / "artwork.png"
    artwork.write_bytes(b"png")
    job = started(
        {"SNDS_TEST_HOLD_STAGE": stage, "SNDS_FAKE_SEGMENTS": "3"},
        argv=[sys.executable, "-I", "-c", HOLD_AT_STAGE],
    )

    job.send(request)
    before = job.events_until(lambda e: e["type"] == "stage" and e["stage"] == stage)
    job.send({"protocol": 1, "type": "cancel"})
    after, code = job.finish()

    events = before + after
    check_stream(job.raw, events)
    assert events[-1]["type"] == "cancelled", events[-1]
    assert events[-1]["reason"] == "requested"
    assert code == jobs_protocol.EXIT_CANCELLED
    assert [e["stage"] for e in events if e["type"] == "stage"][-1] == stage
    assert not (Path(request["jobDir"]) / "package").exists()  # incomplete audio removed
    assert cache_entries(tmp_path) == []  # and nothing was published
    assert artwork.exists()  # the app's own files stay
    assert (home / "logs" / "worker.log").is_file()  # the worker log is kept
    assert "cancelled" in (home / "logs" / "worker.log").read_text("utf-8")


@pytest.mark.parametrize("stage", ["decoding", "separating", "finalizing"])
def test_closing_stdin_at_a_stage_cancels(tmp_path: Path, started, stage: str) -> None:
    request = request_for_source(write_wav(tmp_path / "song.wav", 44_100, 4.0), tmp_path)
    job = started(
        {"SNDS_TEST_HOLD_STAGE": stage, "SNDS_FAKE_SEGMENTS": "3"},
        argv=[sys.executable, "-I", "-c", HOLD_AT_STAGE],
    )

    job.send(request)
    before = job.events_until(lambda e: e["type"] == "stage" and e["stage"] == stage)
    job.close_stdin()
    after, code = job.finish()

    events = before + after
    check_stream(job.raw, events)
    assert (events[-1]["type"], events[-1]["reason"]) == ("cancelled", "stdin_closed")
    assert code == jobs_protocol.EXIT_CANCELLED
    assert cache_entries(tmp_path) == []


def test_cancel_takes_effect_promptly_during_separation(tmp_path: Path, started) -> None:
    """The app waits a grace period after `cancel` and then kills the process tree; the worker
    must be done well inside it when its work is chopped into small segments."""
    request = request_for_source(write_wav(tmp_path / "song.wav", 44_100, 4.0), tmp_path)
    job = started({"SNDS_FAKE_SEGMENTS": "400", "SNDS_FAKE_SEGMENT_MS": "100"})

    job.send(request)
    job.events_until(lambda e: e["type"] == "progress" and e["stage"] == "separating")
    sent = time.monotonic()
    job.send({"protocol": 1, "type": "cancel"})
    events, code = job.finish()

    assert code == jobs_protocol.EXIT_CANCELLED
    assert events[-1]["type"] == "cancelled"
    assert time.monotonic() - sent < 5.0


# --- Abnormal exit ---------------------------------------------------------------------------


def test_a_killed_worker_leaves_no_playable_entry_and_the_next_job_succeeds(
    tmp_path: Path, started
) -> None:
    source = write_wav(tmp_path / "song.wav", 44_100, 4.0)
    request = request_for_source(source, tmp_path)
    victim = started({"SNDS_FAKE_SEGMENTS": "400", "SNDS_FAKE_SEGMENT_MS": "100"})

    victim.send(request)
    victim.events_until(lambda e: e["type"] == "progress" and e["stage"] == "separating")
    victim.process.kill()  # what TerminateJobObject does after the grace period
    events, code = victim.finish()

    assert code != 0
    assert not [e for e in events if e["type"] in jobs_protocol.TERMINAL_TYPES]
    assert cache_entries(tmp_path) == []  # a kill can never publish
    assert (Path(request["jobDir"]) / "package").is_dir()  # left for the app to quarantine

    retry = dict(request, jobId="job-2", jobDir=str(tmp_path / "jobs" / "job-2"))
    Path(retry["jobDir"]).mkdir(parents=True)
    next_job = started()
    next_job.send(retry)
    events, code = next_job.finish()

    assert code == jobs_protocol.EXIT_OK, events[-1]
    assert [p.name for p in cache_entries(tmp_path)] == [Path(request["cacheDir"]).name]
    assert finalize.verify_package(Path(request["cacheDir"]))[0] == schema.COMPLETE


def test_the_worker_does_not_outlive_its_parent_pipe(tmp_path: Path, started) -> None:
    """If the app dies, stdin and stdout both break; the worker stops by itself."""
    request = request_for_source(write_wav(tmp_path / "song.wav", 44_100, 4.0), tmp_path)
    job = started({"SNDS_FAKE_SEGMENTS": "400", "SNDS_FAKE_SEGMENT_MS": "100"})

    job.send(request)
    job.events_until(lambda e: e["type"] == "progress" and e["stage"] == "separating")
    job.close_stdin()
    assert job.process.stdout is not None
    job.process.stdout.close()

    assert job.process.wait(timeout=60) in (
        jobs_protocol.EXIT_CANCELLED,
        jobs_protocol.EXIT_INTERNAL_ERROR,
    )
    assert cache_entries(tmp_path) == []


# --- Request framing -------------------------------------------------------------------------


def test_the_request_may_arrive_in_pieces_with_crlf(tmp_path: Path, started) -> None:
    request = request_for_source(write_wav(tmp_path / "song.wav", 44_100, 3.0), tmp_path)
    data = json.dumps(request).encode("ascii") + b"\r\n"
    job = started()

    for start in range(0, len(data), 40):
        job.send(data[start : start + 40])
        time.sleep(0.02)
    events, code = job.finish()

    check_stream(job.raw, events)
    assert code == jobs_protocol.EXIT_OK, events[-1]


def test_non_ascii_paths_survive_the_ascii_json_protocol(tmp_path: Path, started) -> None:
    folder = tmp_path / "Música 曲"
    folder.mkdir()
    source = write_wav(folder / "Ünï.wav", 44_100, 3.0)
    request = request_for_source(source, tmp_path)
    job = started()

    job.send(request)
    events, code = job.finish()

    check_stream(job.raw, events)
    assert code == jobs_protocol.EXIT_OK, events[-1]
    data = json.loads((Path(request["cacheDir"]) / "song.json").read_text(encoding="utf-8"))
    assert data["source"]["fileName"] == "Ünï.wav"
    assert data["source"]["path"] == str(source)


@pytest.mark.parametrize("version", [0, -1, 2, 99])
def test_unknown_protocol_versions_are_refused_before_any_work(started, version: int) -> None:
    job = started()

    job.send({"protocol": version, "type": "prepare", "jobId": "x"})
    events, code = job.finish()

    assert [e["type"] for e in events] == ["failure"]
    assert events[0]["code"] == "unsupported_protocol_version"
    assert events[0]["supportedProtocols"] == [1]
    assert code == jobs_protocol.EXIT_UNSUPPORTED_PROTOCOL


def test_a_source_that_changed_is_reported_over_the_wire(tmp_path: Path, started) -> None:
    source = write_wav(tmp_path / "song.wav", 44_100, 3.0)
    request = request_for_source(source, tmp_path)
    source.write_bytes(source.read_bytes()[:-1] + b"\x00")  # same size, other content
    job = started()

    job.send(request)
    events, code = job.finish()

    check_stream(job.raw, events)
    assert (events[-1]["type"], events[-1]["code"]) == ("failure", "source_changed")
    assert code == jobs_protocol.EXIT_JOB_FAILED
    assert cache_entries(tmp_path) == []


def test_unsupported_audio_is_reported_over_the_wire(tmp_path: Path, started) -> None:
    surround = tmp_path / "surround.wav"
    sf.write(str(surround), synth_audio(44_100, 1.0, 2).repeat(3, axis=1)[:, :6], 44_100)
    request = request_for_source(surround, tmp_path)
    job = started()

    job.send(request)
    events, code = job.finish()

    assert events[-1]["code"] == "source_unsupported"
    assert "6 channels" in events[-1]["message"]
    assert code == jobs_protocol.EXIT_JOB_FAILED
