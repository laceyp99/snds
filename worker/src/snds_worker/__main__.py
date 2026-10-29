"""Entry point: ``python -u -X utf8 -m snds_worker <command>``. See docs/protocol.md."""

from snds_worker.cli import main

if __name__ == "__main__":
    raise SystemExit(main())
