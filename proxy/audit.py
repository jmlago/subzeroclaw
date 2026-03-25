"""Append-only JSONL audit logger for proxy requests."""

import json
import os
import threading
import time
from pathlib import Path
from typing import Any

SZC_HOME = Path(os.environ.get("SZC_HOME", "/agent-data"))
AUDIT_PATH = SZC_HOME / "vault" / "proxy-audit.jsonl"
MAX_LINES = 50_000
KEEP_LINES = 10_000

_lock = threading.Lock()


def log_request(
    server_id: str,
    method: str,
    path: str,
    status_code: int,
    latency_ms: float,
    source_ip: str,
) -> None:
    """Append an audit entry and rotate if needed."""
    entry = {
        "ts": time.time(),
        "server_id": server_id,
        "method": method,
        "path": path,
        "status_code": status_code,
        "latency_ms": round(latency_ms, 2),
        "source_ip": source_ip,
    }
    with _lock:
        AUDIT_PATH.parent.mkdir(parents=True, exist_ok=True)
        with open(AUDIT_PATH, "a") as f:
            f.write(json.dumps(entry) + "\n")
        _maybe_rotate()


def _maybe_rotate() -> None:
    """Truncate to last KEEP_LINES entries if file exceeds MAX_LINES."""
    try:
        with open(AUDIT_PATH, "r") as f:
            lines = f.readlines()
        if len(lines) > MAX_LINES:
            with open(AUDIT_PATH, "w") as f:
                f.writelines(lines[-KEEP_LINES:])
    except FileNotFoundError:
        pass


def get_recent(limit: int = 100) -> list[dict[str, Any]]:
    """Return the last *limit* audit entries, newest first."""
    try:
        with _lock:
            with open(AUDIT_PATH, "r") as f:
                lines = f.readlines()
        entries = []
        for line in lines[-limit:]:
            line = line.strip()
            if line:
                entries.append(json.loads(line))
        entries.reverse()
        return entries
    except FileNotFoundError:
        return []
