import json
import subprocess
import sys

import snds_worker


def test_version_is_exposed() -> None:
    assert snds_worker.__version__ == "0.1.0"


def test_module_entry_point_emits_one_json_line() -> None:
    proc = subprocess.run(
        [sys.executable, "-m", "snds_worker"], capture_output=True, text=True, check=True
    )
    lines = proc.stdout.splitlines()
    assert len(lines) == 1
    assert json.loads(lines[0]) == {"type": "hello", "worker_version": "0.1.0"}
