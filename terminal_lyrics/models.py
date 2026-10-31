from dataclasses import dataclass
import math


@dataclass(frozen=True)
class Track:
    title: str
    artist: str
    album: str = ""
    player: str = ""
    duration: float = 0.0
    track_id: str = ""

    @property
    def key(self) -> tuple[str, str, str]:
        return (self.title.casefold(), self.artist.casefold(), self.album.casefold())

    @property
    def lyrics_key(self) -> tuple[str, str, str, int]:
        # Reuse a song across players, while separating recordings of different lengths.
        duration = round(self.duration) if math.isfinite(self.duration) and self.duration > 0 else 0
        return (*self.key, duration)
