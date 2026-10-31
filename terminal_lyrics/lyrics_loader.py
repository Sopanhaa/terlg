from collections import OrderedDict
from threading import Condition, Event, Thread

from .lyrics_service import fetch_lyrics
from .models import Track

LyricsResult = tuple[list[tuple[float, str]], list[str], str]


class LyricsLoader:
    """Load the latest song off the UI thread and reuse successful lookups."""

    def __init__(self, cache_size: int = 64) -> None:
        self._condition = Condition()
        self._cache_size = max(0, cache_size)
        self._cache: OrderedDict[tuple[str, str, str, int], LyricsResult] = OrderedDict()
        self._pending: tuple[Track, Event] | None = None
        self._active: Event | None = None
        self._result: tuple[tuple[str, str, str, int], LyricsResult] | None = None
        self._closed = False
        # A slow HTTP request must not delay quitting the terminal application.
        self._worker = Thread(target=self._run, name="lyrics-loader", daemon=True)
        self._worker.start()

    def request(self, track: Track) -> None:
        with self._condition:
            if self._closed:
                return
            if self._active is not None:
                self._active.set()
            self._pending = None
            self._result = None
            cached = self._cache.get(track.lyrics_key)
            if cached is not None:
                self._cache.move_to_end(track.lyrics_key)
                self._result = (track.lyrics_key, cached)
                return
            self._active = Event()
            self._pending = (track, self._active)
            self._condition.notify()

    def poll(self, track: Track) -> LyricsResult | None:
        with self._condition:
            if self._result is None or self._result[0] != track.lyrics_key:
                return None
            _, result = self._result
            self._result = None
            return result

    def close(self) -> None:
        with self._condition:
            self._closed = True
            self._pending = None
            if self._active is not None:
                self._active.set()
            self._condition.notify()

    def _run(self) -> None:
        while True:
            with self._condition:
                self._condition.wait_for(lambda: self._closed or self._pending is not None)
                if self._closed:
                    return
                track, cancelled = self._pending
                self._pending = None
            try:
                result = fetch_lyrics(track, cancel_event=cancelled)
            except Exception as exc:
                result = ([], [], f"Could not load lyrics: {exc}")
            with self._condition:
                if self._closed:
                    return
                if cancelled.is_set():
                    continue
                if (result[0] or result[1]) and self._cache_size:
                    self._cache[track.lyrics_key] = result
                    self._cache.move_to_end(track.lyrics_key)
                    while len(self._cache) > self._cache_size:
                        self._cache.popitem(last=False)
                self._result = (track.lyrics_key, result)
