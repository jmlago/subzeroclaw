"""Thread-safe in-memory token-bucket rate limiter per server_id."""

import re
import threading
import time

_UNIT_SECONDS = {
    "second": 1,
    "minute": 60,
    "hour": 3600,
    "day": 86400,
}

_RATE_RE = re.compile(r"^(\d+)\s*/\s*(second|minute|hour|day)$", re.IGNORECASE)


class _Bucket:
    __slots__ = ("capacity", "refill_rate", "tokens", "last_refill")

    def __init__(self, capacity: int, period_seconds: float) -> None:
        self.capacity = capacity
        self.refill_rate = capacity / period_seconds  # tokens per second
        self.tokens = float(capacity)
        self.last_refill = time.monotonic()

    def allow(self) -> bool:
        now = time.monotonic()
        elapsed = now - self.last_refill
        self.tokens = min(self.capacity, self.tokens + elapsed * self.refill_rate)
        self.last_refill = now
        if self.tokens >= 1.0:
            self.tokens -= 1.0
            return True
        return False


class RateLimiter:
    """Per-server token-bucket rate limiter."""

    def __init__(self) -> None:
        self._buckets: dict[str, _Bucket] = {}
        self._lock = threading.Lock()

    @staticmethod
    def parse_rate(rate_str: str) -> tuple[int, float]:
        """Parse '60/minute' -> (capacity=60, period_seconds=60.0)."""
        m = _RATE_RE.match(rate_str.strip())
        if not m:
            raise ValueError(f"Invalid rate format: {rate_str!r}. Expected e.g. '60/minute'.")
        capacity = int(m.group(1))
        unit = m.group(2).lower()
        return capacity, float(_UNIT_SECONDS[unit])

    def allow(self, server_id: str, rate_str: str | None = None) -> bool:
        """Check whether *server_id* is within its rate limit."""
        with self._lock:
            bucket = self._buckets.get(server_id)
            if bucket is None:
                if rate_str is None:
                    return True  # no rate configured -> always allow
                capacity, period = self.parse_rate(rate_str)
                bucket = _Bucket(capacity, period)
                self._buckets[server_id] = bucket
            return bucket.allow()
