import json
import subprocess
import sys

import snds_worker


def _run(*args: str) -> tuple[int, list[dict]]:
    proc = subprocess.run(
        [sys.executable, "-u", "-X", "utf8", "-m", "snds_worker", *args],
        capture_output=True,
        stdin=subprocess.DEVNULL,
        check=False,
    )
    assert b"\r\n" not in proc.stdout, "protocol lines must end in LF only"
    return proc.returncode, [json.loads(line) for line in proc.stdout.decode().splitlines()]


def test_version_is_exposed() -> None:
    assert snds_worker.__version__ == "0.1.0"


def test_version_command_emits_hello_and_result() -> None:
    code, msgs = _run("version", "--protocol-version", "1")
    assert code == 0
    assert [m["type"] for m in msgs] == ["hello", "result"]
    assert msgs[0]["worker_version"] == "0.1.0"
    assert msgs[0]["protocol_versions"] == [1]
    assert msgs[1]["exit_code"] == 0
    assert all(m["protocol_version"] == 1 for m in msgs)
    assert [m["seq"] for m in msgs] == [1, 2]


def test_no_command_is_a_bad_invocation() -> None:
    code, msgs = _run()
    assert code == 2
    assert msgs[-1]["type"] == "failed"
    assert msgs[-1]["error"]["code"] == "bad-invocation"
    assert msgs[-1]["exit_code"] == 2


def test_unknown_protocol_version_on_the_command_line_is_rejected() -> None:
    code, msgs = _run("version", "--protocol-version", "2")
    assert code == 2
    assert msgs[-1]["error"]["code"] == "unsupported-protocol-version"
    assert msgs[-1]["error"]["detail"] == {"requested": 2, "supported": [1]}
