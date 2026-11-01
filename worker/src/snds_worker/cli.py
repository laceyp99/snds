"""Command-line entry point: python -m snds_worker <command>.

Exit codes: 0 success, 1 the command ran and reported a failure (for example "not ready"),
2 usage error, 3 unexpected internal error. Every command except `prepare` and a usage error
prints exactly one JSON object on stdout; `prepare` streams NDJSON events and has its own exit
codes (see jobs_protocol).
"""

from __future__ import annotations

import argparse
import http.client
import logging
from collections.abc import Callable, Sequence
from typing import Any

from snds_worker import model, protocol, readiness
from snds_worker.logs import configure_logging
from snds_worker.paths import HomeUnavailableError, data_paths

EXIT_OK = 0
EXIT_FAILED = 1
EXIT_INTERNAL = 3  # argparse uses 2 for usage errors

Result = tuple[int, dict[str, Any]]

log = logging.getLogger(__name__)


def _readiness(_args: argparse.Namespace) -> Result:
    report = readiness.check()
    return (EXIT_OK if report["ready"] else EXIT_FAILED), report


def _fetch_model(_args: argparse.Namespace) -> Result:
    try:
        paths = data_paths()
    except HomeUnavailableError as error:
        return EXIT_FAILED, protocol.message(
            "fetch-model", ok=False, problems=[protocol.problem("home_unavailable", str(error))]
        )

    configure_logging(paths.logs)
    checkpoints = (
        (
            "htdemucs",
            model.model_path(paths),
            model.MODEL_URL,
            model.MODEL_SHA256,
            model.MODEL_SIZE,
        ),
        (
            "beat_this",
            model.beat_model_path(paths),
            model.BEAT_MODEL_URL,
            model.BEAT_MODEL_SHA256,
            model.BEAT_MODEL_SIZE,
        ),
    )

    problems: list[dict[str, str]] = []
    fetched: list[dict[str, Any]] = []
    for name, destination, url, sha256, size in checkpoints:
        try:
            result = model.fetch(destination, url, sha256, size)
        except model.ModelVerificationError as error:
            log.error("%s checkpoint verification failed: %s", name, error)
            problems.append(protocol.problem("model_verification_failed", f"{name}: {error}"))
            continue
        except (OSError, http.client.HTTPException) as error:
            log.error("%s checkpoint download failed: %s", name, error)
            problems.append(
                protocol.problem(
                    "download_failed", f"Could not download the {name} checkpoint: {error}"
                )
            )
            continue
        fetched.append(
            {
                "name": name,
                "path": str(result.path),
                "downloaded": result.downloaded,
                "size_bytes": result.size,
                "sha256": result.sha256,
            }
        )

    # The top-level path/downloaded/size_bytes/sha256 describe htdemucs, as in earlier builds;
    # `models` lists every checkpoint.
    first = next((entry for entry in fetched if entry["name"] == "htdemucs"), None)
    top = (
        {key: first[key] for key in ("path", "downloaded", "size_bytes", "sha256")}
        if first
        else {"path": str(checkpoints[0][1])}
    )
    return (EXIT_FAILED if problems else EXIT_OK), protocol.message(
        "fetch-model", ok=not problems, **top, models=fetched, problems=problems
    )


COMMANDS: dict[str, Callable[[argparse.Namespace], Result]] = {
    "readiness": _readiness,
    "fetch-model": _fetch_model,
}


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m snds_worker",
        description="SNDS worker. Each command prints exactly one JSON object on stdout.",
    )
    commands = parser.add_subparsers(dest="command", required=True, metavar="<command>")
    commands.add_parser(
        "prepare", help="run one preparation job (request on stdin, NDJSON events on stdout)"
    )
    commands.add_parser("readiness", help="report whether this environment can run separation")
    commands.add_parser(
        "fetch-model", help="download and verify the pinned htdemucs and beat_this checkpoints"
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    protocol.ensure_standard_streams()
    args = build_parser().parse_args(argv)
    configure_logging()

    if args.command == "prepare":  # a long-running job with its own stdout protocol
        from snds_worker import prepare  # heavy imports wait until the standard streams exist

        return prepare.run()

    try:
        with protocol.stdout_to_stderr():
            exit_code, payload = COMMANDS[args.command](args)
    except Exception as error:
        log.exception("%s failed unexpectedly", args.command)
        exit_code = EXIT_INTERNAL
        payload = protocol.message(
            "error",
            command=args.command,
            problems=[protocol.problem("internal_error", f"{type(error).__name__}: {error}")],
        )

    protocol.emit(payload)
    return exit_code
