from __future__ import annotations

import hashlib
import http.server
import threading
from pathlib import Path

import pytest

from conftest import FAKE_ENV, link_model
from snds_worker.client import run_worker
from snds_worker.model import (
    BAG_YAML,
    CHECKPOINT_FILE,
    MEMO_FILE,
    MODEL_NAME,
    download_file,
    verify_checkpoint,
)
from snds_worker.protocol import ErrorCode, WorkerError

STATUSES = {"ok", "warning", "missing"}


def _readiness(data_root: Path, *args: str, env: dict | None = None) -> dict:
    res = run_worker(
        "readiness",
        ["--protocol-version", "1", "--data-root", str(data_root), *args],
        env=env,
        timeout=180,
    )
    assert res.exit_code == 0, res.stderr
    assert res.consistent and [m["type"] for m in res.messages] == ["hello", "result"]
    report = res.terminal["readiness"]
    for c in report["checks"]:
        assert set(c) == {"id", "title", "status", "detail", "action", "data"}
        assert c["status"] in STATUSES
        if c["status"] != "ok":
            assert c["action"] or c["id"] == "cuda"
    return report


def _check(report: dict, id_: str) -> dict:
    return next(c for c in report["checks"] if c["id"] == id_)


def test_readiness_without_model(data_root: Path) -> None:
    report = _readiness(data_root, "--force-cpu")
    ids = [c["id"] for c in report["checks"]]
    assert ids == [
        "worker-python",
        "worker-packages",
        "mp3-decoder",
        "data-directories",
        "model-checkpoint",
        "cpu",
        "cuda",
    ]
    model = _check(report, "model-checkpoint")
    assert model["status"] == "missing" and "bootstrap" in model["action"]
    assert model["data"]["present"] is False
    assert report["separation_available"] is False
    assert report["compute"] == {
        "requested": "cpu",
        "selected": "cpu",
        "reason": "forced-cpu",
        "device_name": None,
    }
    assert _check(report, "worker-packages")["status"] == "ok"
    assert _check(report, "mp3-decoder")["status"] == "ok"
    assert _check(report, "data-directories")["data"]["same_volume"] is True
    assert report["pipeline_id"].startswith("snds-p1-htdemucs-")


def test_readiness_reports_a_corrupt_model(data_root: Path) -> None:
    d = data_root / "models" / MODEL_NAME
    d.mkdir(parents=True)
    (d / CHECKPOINT_FILE).write_bytes(b"not a checkpoint")
    model = _check(_readiness(data_root, "--force-cpu"), "model-checkpoint")
    assert model["status"] == "missing" and "size" in model["data"]["problem"]


def test_readiness_flags_test_mode(data_root: Path) -> None:
    report = _readiness(data_root, "--force-cpu", env=FAKE_ENV)
    assert _check(report, "worker-test-mode")["status"] == "warning"
    assert report["pipeline_id"] == "snds-test-fake-separator"


@pytest.mark.model
def test_readiness_verifies_the_model_and_memoizes(model_root: Path) -> None:
    first = _check(_readiness(model_root, "--force-cpu"), "model-checkpoint")
    assert first["status"] == "ok" and first["data"]["verified_via"] == "hash"
    assert (model_root / "models" / MODEL_NAME / MEMO_FILE).is_file()
    second = _check(_readiness(model_root, "--force-cpu"), "model-checkpoint")
    assert second["data"]["verified_via"] == "memo"
    assert second["data"]["sha256"] == first["data"]["sha256"]
    forced = _check(_readiness(model_root, "--force-cpu", "--full-verify"), "model-checkpoint")
    assert forced["data"]["verified_via"] == "hash"


@pytest.mark.cuda
def test_readiness_cuda_health_check(model_root: Path) -> None:
    report = _readiness(model_root)
    cuda = _check(report, "cuda")
    assert cuda["status"] == "ok" and cuda["data"]["probe"] == "passed"
    assert report["compute"]["selected"] == "cuda:0"
    assert report["separation_available"] is True


@pytest.mark.cuda
def test_readiness_cuda_probe_failure_falls_back(data_root: Path) -> None:
    report = _readiness(data_root, env={"SNDS_WORKER_TEST_FAULT": "cuda-probe"})
    cuda = _check(report, "cuda")
    assert cuda["status"] == "warning" and cuda["data"]["probe"] == "failed"
    assert (
        report["compute"]["selected"] == "cpu"
        and report["compute"]["reason"] == "cuda-probe-failed"
    )


# --- fetch-model -----------------------------------------------------------------------


class _Server:
    """Serves ``payload``; the first ``short`` responses send only half of it, then close."""

    def __init__(self, payload: bytes, status: int = 200, short: int = 0) -> None:
        payload_ = payload
        remaining_short = [short]

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self) -> None:  # noqa: N802
                self.send_response(status)
                self.send_header("Content-Length", str(len(payload_)))
                self.end_headers()
                if remaining_short[0] > 0:
                    remaining_short[0] -= 1
                    self.wfile.write(payload_[: len(payload_) // 2])
                    self.close_connection = True
                    return
                self.wfile.write(payload_)

            def log_message(self, *args) -> None:
                pass

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.url = f"http://127.0.0.1:{self.httpd.server_address[1]}/model.th"
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self) -> None:
        self.httpd.shutdown()
        self.httpd.server_close()


def test_download_verifies_and_renames_atomically(tmp_path: Path) -> None:
    payload = b"x" * 3_000_000
    srv = _Server(payload)
    try:
        target = tmp_path / "m.th"
        seen: list[int] = []
        info = download_file(
            srv.url,
            target,
            expected_size=len(payload),
            expected_sha256=hashlib.sha256(payload).hexdigest(),
            progress=lambda done, total: seen.append(done),
        )
        assert target.read_bytes() == payload and info["attempts"] == 1
        assert seen[-1] == len(payload)
        assert sorted(p.name for p in tmp_path.iterdir()) == ["m.th"]
        with pytest.raises(WorkerError) as ei:
            download_file(
                srv.url, tmp_path / "bad.th", expected_size=len(payload), expected_sha256="0" * 64
            )
        assert ei.value.code == ErrorCode.MODEL_DOWNLOAD_FAILED
        assert sorted(p.name for p in tmp_path.iterdir()) == ["m.th"]  # no .part, no target
    finally:
        srv.close()


def test_download_retries_a_truncated_response(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr("snds_worker.model.time.sleep", lambda _s: None)
    payload = b"y" * 1_000_000
    srv = _Server(payload, short=1)
    try:
        target = tmp_path / "m.th"
        info = download_file(
            srv.url,
            target,
            expected_size=len(payload),
            expected_sha256=hashlib.sha256(payload).hexdigest(),
        )
        assert info["attempts"] == 2 and target.read_bytes() == payload
        assert sorted(p.name for p in tmp_path.iterdir()) == ["m.th"]
    finally:
        srv.close()


def test_download_http_error(tmp_path: Path) -> None:
    srv = _Server(b"nope", status=404)
    try:
        with pytest.raises(WorkerError) as ei:
            download_file(
                srv.url, tmp_path / "m.th", expected_size=4, expected_sha256="0" * 64, attempts=1
            )
        assert ei.value.code == ErrorCode.MODEL_DOWNLOAD_FAILED
        assert list(tmp_path.iterdir()) == []
    finally:
        srv.close()


@pytest.mark.model
def test_fetch_model_is_idempotent_when_installed(data_root: Path) -> None:
    link_model(data_root)
    res = run_worker("fetch-model", ["--protocol-version", "1", "--data-root", str(data_root)])
    assert res.exit_code == 0 and res.consistent, res.stderr
    model = res.terminal["model"]
    assert model["downloaded"] is False and model["yaml_written"] is True
    d = data_root / "models" / MODEL_NAME
    assert (d / "htdemucs.yaml").read_text() == BAG_YAML
    assert verify_checkpoint(data_root / "models").verified_via == "memo"
    again = run_worker("fetch-model", ["--data-root", str(data_root)])
    assert again.exit_code == 0 and again.terminal["model"]["yaml_written"] is False
    assert [m["stage"] for m in again.of_type("stage")] == ["checking-model", "installing"]
