"""SNDS worker: a short-lived helper process launched by the SNDS app."""

from importlib import metadata

try:
    __version__ = metadata.version("snds-worker")
except metadata.PackageNotFoundError:  # imported from a source tree without installation
    __version__ = "0+unknown"
