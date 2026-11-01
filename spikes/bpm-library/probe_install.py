"""Probe whether BPM candidates install beside the pinned separation stack.

Each candidate gets a scratch uv project (base pins + candidate) outside the
repository. The probe locks it, then syncs it with ``--locked`` while
forbidding source builds for every package the candidate adds
(``--no-build-package``) to mimic an end-user machine without a C/C++
toolchain. ``--no-build`` cannot be used globally because ``demucs==4.0.1``
itself is published only as a pure-Python sdist. When the wheel-only sync
fails, a second sync with builds allowed records whether the package would
build on a developer machine (this one has Visual Studio 2022).

Run with any Python 3.11+; only the standard library is used. Results are
printed as ``key=value`` lines prefixed with the candidate name.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time
import tomllib

BASE_DEPENDENCIES = [
    "demucs==4.0.1",
    "numpy==1.26.4",
    "packaging==25.0",
    "soundfile==0.12.1",
    "torch==2.1.2",
    "torchaudio==2.1.2",
]
PINNED = {
    "demucs": "4.0.1",
    "numpy": "1.26.4",
    "torch": "2.1.2",
    "torchaudio": "2.1.2",
}
MADMOM_COMMIT = "27f032e8947204902c675e5e341a3faf5dc86dae"

# name -> (requirements, import smoke statement, lock_only)
CANDIDATES: dict[str, tuple[list[str], str, bool]] = {
    "librosa": (["librosa"], "import librosa; print(librosa.__version__)", False),
    "aubio": (["aubio==0.4.9"], "import aubio; print(aubio.version)", False),
    "madmom": (["madmom==0.16.1"], "import madmom; print(madmom.__version__)", False),
    "madmom-git": (
        [f"madmom @ git+https://github.com/CPJKU/madmom@{MADMOM_COMMIT}"],
        "import madmom; print(madmom.__version__)",
        False,
    ),
    "essentia": (["essentia"], "import essentia; print(essentia.__version__)", False),
    "deeprhythm": (
        ["deeprhythm==0.0.13"],
        "import deeprhythm, importlib.metadata as m; print(m.version('deeprhythm'))",
        False,
    ),
    "beat-this": (
        ["beat-this==1.1.0"],
        "import beat_this, importlib.metadata as m; print(m.version('beat-this'))",
        False,
    ),
    "tempocnn": (["tempocnn==0.0.8"], "", True),
    "beatnet": (["BeatNet==1.1.3"], "", True),
}

PYPROJECT_TEMPLATE = """[project]
name = "snds-bpm-probe-{name}"
version = "0.0.0"
requires-python = "==3.11.*"
dependencies = [
{deps}
]

[[tool.uv.index]]
name = "pytorch-cpu"
url = "https://download.pytorch.org/whl/cpu"
explicit = true

[tool.uv.sources]
torch = {{ index = "pytorch-cpu" }}
torchaudio = {{ index = "pytorch-cpu" }}
"""


def spikes_root() -> pathlib.Path:
    return pathlib.Path(os.environ["LOCALAPPDATA"]) / "snds-spikes"


def run(cmd: list[str], env: dict[str, str], timeout: int) -> tuple[int, str, float]:
    started = time.perf_counter()
    try:
        done = subprocess.run(
            cmd,
            env=env,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
        )
        code, output = done.returncode, done.stdout + done.stderr
    except subprocess.TimeoutExpired as error:
        code, output = -1, f"timeout after {timeout}s: {error}"
    return code, output, time.perf_counter() - started


def last_error_line(output: str) -> str:
    lines = [line.strip() for line in output.splitlines() if line.strip()]
    tokens = ("error", "failed", "unsatisfiable", "because", "not found", "can't", "cannot")
    interesting = [line for line in lines if any(t in line.lower() for t in tokens)]
    picked = (interesting or lines or ["<no output>"])[-3:]
    return " | ".join(picked)[:600]


def directory_bytes(path: pathlib.Path) -> int:
    total = 0
    for root, _dirs, files in os.walk(path):
        for name in files:
            try:
                total += (pathlib.Path(root) / name).stat().st_size
            except OSError:
                pass
    return total


def lock_data(lock_path: pathlib.Path) -> dict[str, dict]:
    data = tomllib.loads(lock_path.read_text(encoding="utf-8"))
    return {pkg["name"]: pkg for pkg in data.get("package", [])}


def windows_artifact(pkg: dict) -> tuple[str, int]:
    """Classify the artifact uv would install on Windows x64 CPython 3.11."""
    wheels = [(w["url"].rsplit("/", 1)[-1], w.get("size", 0)) for w in pkg.get("wheels", [])]
    for name, size in wheels:
        tags = name.removesuffix(".whl").split("-")[-3:]
        if tags[2] == "win_amd64" and (tags[0] in ("cp311", "py3", "py2.py3") or tags[1] == "abi3"):
            return "wheel-win", size
    for name, size in wheels:
        if name.endswith("-none-any.whl"):
            return "wheel-any", size
    if "sdist" in pkg:
        return "sdist-only", pkg["sdist"].get("size", 0)
    if "git" in str(pkg.get("source", "")):
        return "git-source", 0
    return "none-for-windows", 0


def installed_bytes(environment: pathlib.Path, names: list[str]) -> int:
    """Sum the file sizes listed in the RECORD of each given distribution."""
    site = environment / "Lib" / "site-packages"
    wanted = {re.sub(r"[-_.]+", "_", name.lower()) for name in names}
    total = 0
    for dist_info in site.glob("*.dist-info"):
        stem = dist_info.name.removesuffix(".dist-info")
        project = re.sub(r"[-_.]+", "_", stem.rsplit("-", 1)[0].lower())
        record = dist_info / "RECORD"
        if project not in wanted or not record.exists():
            continue
        for line in record.read_text(encoding="utf-8").splitlines():
            parts = line.rsplit(",", 2)
            if len(parts) != 3:
                continue
            if parts[2].isdigit():
                total += int(parts[2])
            elif (site / parts[0]).is_file():
                total += (site / parts[0]).stat().st_size
    return total


def probe(name: str, base_packages: set[str], timeout: int) -> None:
    requirements, smoke, lock_only = CANDIDATES[name]
    project = spikes_root() / "bpm-probe-projects" / name
    environment = spikes_root() / f"bpm-probe-{name}"
    for path in (project, environment):
        if path.exists():
            shutil.rmtree(path)
    project.mkdir(parents=True)
    deps = ",\n".join(f'    "{dep}"' for dep in BASE_DEPENDENCIES + requirements)
    (project / "pyproject.toml").write_text(
        PYPROJECT_TEMPLATE.format(name=name, deps=deps), encoding="utf-8"
    )
    env = dict(os.environ)
    env["UV_PROJECT_ENVIRONMENT"] = str(environment)
    env.pop("VIRTUAL_ENV", None)

    print(f"[{name}] requirements={';'.join(requirements)}")
    code, output, seconds = run(["uv", "lock", "--project", str(project), "--python", "3.11"], env, timeout)
    print(f"[{name}] lock_exit={code} lock_seconds={seconds:.1f}")
    if code != 0:
        print(f"[{name}] lock_error={last_error_line(output)}")
        print(f"[{name}] result=lock-failed")
        return

    data = lock_data(project / "uv.lock")
    packages = {key: pkg.get("version", "?") for key, pkg in data.items()}
    locked_pins = {key: packages.get(key, "?").split("+")[0] for key in PINNED}
    extra = sorted(set(packages) - base_packages - {f"snds-bpm-probe-{name}"})
    candidate_key = re.split(r"[=@ ]", requirements[0])[0].lower().replace("_", "-")
    artifacts = {key: windows_artifact(data[key]) for key in extra}
    download = sum(size for _kind, size in artifacts.values())
    needs_build = sorted(k for k, (kind, _s) in artifacts.items() if kind not in ("wheel-win", "wheel-any"))
    print(f"[{name}] locked_candidate_version={packages.get(candidate_key, '?')}")
    print(f"[{name}] locked_separation_stack={','.join(f'{k}=={packages.get(k)}' for k in PINNED)}")
    print(f"[{name}] separation_pins_unchanged={locked_pins == PINNED}")
    print(f"[{name}] added_packages={len(extra)}")
    print(f"[{name}] added_download_mib={download / 2**20:.1f}")
    print(f"[{name}] added_without_windows_wheel={','.join(needs_build) or 'none'}")
    print(f"[{name}] added_package_list={','.join(f'{p}=={packages[p]}' for p in extra)}")
    if lock_only:
        print(f"[{name}] result=lock-only (install skipped by design)")
        return

    sync = ["uv", "sync", "--project", str(project), "--locked", "--python", "3.11"]
    no_build = [arg for key in extra for arg in ("--no-build-package", key)]
    code, output, seconds = run(sync + no_build, env, timeout)
    print(f"[{name}] wheel_only_sync_exit={code} wheel_only_sync_seconds={seconds:.1f}")
    built_from_source = False
    if code != 0:
        print(f"[{name}] wheel_only_sync_error={last_error_line(output)}")
        code, output, seconds = run(sync, env, timeout)
        built_from_source = True
        print(f"[{name}] source_build_sync_exit={code} source_build_sync_seconds={seconds:.1f}")
        if code != 0:
            print(f"[{name}] source_build_sync_error={last_error_line(output)}")
            print(f"[{name}] result=install-failed")
            return

    python = environment / "Scripts" / "python.exe"
    code, output, seconds = run([str(python), "-c", smoke], env, timeout)
    print(f"[{name}] import_exit={code} import_seconds={seconds:.1f}")
    shown = last_error_line(output) if code else (output.strip().splitlines() or ["?"])[-1]
    print(f"[{name}] import_output={shown}")
    check = (
        "import torch, torchaudio, numpy, demucs;"
        "print(torch.__version__, torchaudio.__version__, numpy.__version__, demucs.__version__)"
    )
    _code, output, _seconds = run([str(python), "-c", check], env, timeout)
    print(f"[{name}] installed_stack={output.strip()}")
    print(f"[{name}] env_mib={directory_bytes(environment) / 2**20:.0f}")
    print(f"[{name}] added_installed_mib={installed_bytes(environment, extra) / 2**20:.1f}")
    if code:
        status = "import-failed"
    elif built_from_source:
        status = "installed-needs-compiler"
    else:
        status = "installed-wheels-only"
    print(f"[{name}] result={status}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("candidates", nargs="*", default=list(CANDIDATES))
    parser.add_argument(
        "--base-lock",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[1] / "uv-cpu-env" / "uv.lock",
    )
    parser.add_argument("--timeout", type=int, default=1800)
    args = parser.parse_args()

    base_packages = set(lock_data(args.base_lock))
    print(f"python={sys.version.split()[0]}")
    print(f"base_packages={len(base_packages)}")
    for name in args.candidates:
        probe(name, base_packages, args.timeout)
        sys.stdout.flush()


if __name__ == "__main__":
    main()
