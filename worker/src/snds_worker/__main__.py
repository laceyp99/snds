"""Placeholder entry point: ``python -m snds_worker``.

Emits one newline-delimited JSON line on stdout describing the worker and exits 0.
The real protocol (readiness, job, progress, cancellation, result) lands in Phase 2.
"""

import json
import sys

from snds_worker import __version__


def main() -> int:
    sys.stdout.write(json.dumps({"type": "hello", "worker_version": __version__}) + "\n")
    sys.stdout.flush()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
