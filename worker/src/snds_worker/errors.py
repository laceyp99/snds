"""Exceptions that end a `prepare` job with a protocol terminal event."""

from __future__ import annotations


class JobFailure(Exception):
    """A job failure with a stable code from docs/protocol.md (a `failure` event, exit 13)."""

    def __init__(
        self,
        code: str,
        message: str,
        *,
        details: str | None = None,
        retryable: bool = False,
        suggested_action: str | None = None,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.details = details
        self.retryable = retryable
        self.suggested_action = suggested_action


class JobCancelled(Exception):
    """The app asked the job to stop (a `cancelled` event, exit 12)."""

    def __init__(self, reason: str = "requested") -> None:
        super().__init__(reason)
        self.reason = reason
