import math
import time

from .models import Track


class PlaybackClock:
    """Advance between player samples, then follow authoritative seek/pause data."""

    def __init__(self) -> None:
        self._identity: tuple | None = None
        self._position: float | None = None
        self._sampled_at = 0.0
        self._playing = False
        self._duration = 0.0

    def update(
        self, track: Track, position: float | None, status: str, *, now: float | None = None,
    ) -> None:
        now = time.monotonic() if now is None else now
        identity = (track.player, track.track_id, track.key)
        previous = self.position(now=now) if identity == self._identity else None
        valid = position is not None and math.isfinite(position) and position >= 0
        self._position = position if valid else previous
        self._identity = identity
        self._sampled_at = now
        self._playing = status.casefold() == "playing"
        self._duration = track.duration if math.isfinite(track.duration) and track.duration > 0 else 0.0

    def position(self, *, now: float | None = None) -> float | None:
        if self._position is None:
            return None
        now = time.monotonic() if now is None else now
        elapsed = max(0.0, now - self._sampled_at) if self._playing else 0.0
        position = self._position + elapsed
        return min(position, self._duration) if self._duration else position
