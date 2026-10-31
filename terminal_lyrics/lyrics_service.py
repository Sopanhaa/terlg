import json
import re
import time
from math import isfinite
from threading import Event
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen

from .models import Track

APP_VERSION = "0.1.0"
API_URL = "https://lrclib.net/api/search"
USER_AGENT = f"TerminalLyrics/{APP_VERSION} (https://lrclib.net/docs)"
MAX_REQUEST_ATTEMPTS = 3
REQUEST_TIMEOUT = 4.0
SEARCH_TIMEOUT = 15.0
RETRYABLE_HTTP_CODES = {500, 502, 503, 504}
TIME_TAG = re.compile(r"\[(\d{1,2}):(\d{2})(?:\.(\d{1,3}))?\]")
VIDEO_QUALIFIER = re.compile(
    r"\s*[\[(][^)\]]*(?:official|lyrics?|audio|video|visualizer)[^)\]]*[\])]",
    re.IGNORECASE,
)
COVER_DESCRIPTION = re.compile(
    r"\b(?:cover|rendition)\b(?:\s+(?:ft\.?|feat\.?|by)\s+.*)?\s*$",
    re.IGNORECASE,
)


def _normalized(value: str) -> str:
    return "".join(character for character in value.casefold() if character.isalnum())


def _artist_keys(artist: str) -> set[str]:
    # A band may be listed with its founder's name, unlike its video channel.
    without_owner = re.sub(r"^.+?['’]s\s+", "", artist, count=1, flags=re.IGNORECASE)
    return {_normalized(artist), _normalized(without_owner)} - {""}


def _title_matches(record_title: str, title: str) -> bool:
    if _normalized(record_title) == _normalized(title):
        return True
    # A subtitle such as "(Stay High)" can be absent from a video's song title.
    without_subtitle = re.sub(r"\s*[\[(][^)\]]*[\])]\s*$", "", record_title)
    return _normalized(without_subtitle) == _normalized(title)


def search_candidates(track: Track) -> list[tuple[str, str]]:
    cleaned_title = VIDEO_QUALIFIER.sub("", track.title).strip()
    derived: list[tuple[str, str]] = []
    video_metadata = (
        cleaned_title != track.title or "//" in cleaned_title
        or bool(COVER_DESCRIPTION.search(cleaned_title)) or not track.artist.strip()
    )

    if "//" in cleaned_title:
        artist_hint, song_and_channel = cleaned_title.split("//", 1)
        song_title = re.split(r"\s+[–—-]\s+", song_and_channel.strip(), maxsplit=1)[0].strip()
        artist_hint = artist_hint.strip()
        if song_title:
            if artist_hint:
                derived.append((song_title, artist_hint))
            derived.append((song_title, track.artist))
    else:
        title_parts = re.split(r"\s+[–—-]\s+", cleaned_title)
        if len(title_parts) > 1:
            first = title_parts[0].strip()
            if _normalized(first) == _normalized(track.artist):
                video_metadata = True
            if len(title_parts) == 2 and COVER_DESCRIPTION.search(title_parts[1]):
                # "Song - style/original artist Cover ft. guest" is title-first.
                derived.append((first, track.artist))
            else:
                # Ignore trailing uploader names and cover descriptions when an
                # artist-first title already identifies the song.
                song_title = title_parts[1].strip()
                derived.extend([(song_title, first), (song_title, track.artist)])
                if track.artist and _normalized(first) != _normalized(track.artist):
                    derived.append((first, track.artist))

    if not derived:
        derived.append((cleaned_title, track.artist))

    # A dash can be part of a real song name. Keep reliable player metadata
    # first unless the title has a recognizable video/artist-prefix format.
    searches: list[tuple[str, str]] = [] if video_metadata else [(track.title, track.artist)]
    for title, artist in derived:
        if not artist:
            continue
        searches.append((title, artist))
        spaced_artist = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", " ", artist)
        if spaced_artist != artist:
            searches.append((title, spaced_artist))

    # Uploader/channel names are not always the artist used by the lyrics database.
    searches.extend((title, "") for title, _ in derived)
    # The full video metadata is a last resort, rather than the first network call.
    searches.append((track.title, track.artist))

    unique_searches: list[tuple[str, str]] = []
    seen_searches: set[tuple[str, str]] = set()
    for title, artist in searches:
        title, artist = title.strip(), artist.strip()
        key = (title.casefold(), artist.casefold())
        if title and key not in seen_searches:
            unique_searches.append((title, artist))
            seen_searches.add(key)
    return unique_searches


def fetch_lyrics(
    track: Track, *, cancel_event: Event | None = None,
) -> tuple[list[tuple[float, str]], list[str], str]:
    deadline = time.monotonic() + SEARCH_TIMEOUT
    searches = search_candidates(track)
    records: dict[str, tuple[dict, bool]] = {}
    known_artists = [
        (title, _artist_keys(artist)) for title, artist in searches if artist
    ]
    error = ""

    def duration_difference(record: dict) -> float | None:
        if not isfinite(track.duration) or track.duration <= 0:
            return None
        try:
            duration = float(record.get("duration") or 0)
        except (TypeError, ValueError):
            return None
        if not isfinite(duration) or duration <= 0:
            return None
        return abs(duration - track.duration)

    def timing_compatible(record: dict) -> bool:
        difference = duration_difference(record)
        return difference is None or difference <= 2.0

    def cancelled() -> bool:
        return cancel_event is not None and cancel_event.is_set()

    def have_lyrics() -> bool:
        return any(
            str(record.get("plainLyrics") or "").strip()
            or parse_lrc(str(record.get("syncedLyrics") or ""))
            for record, _ in records.values()
        )

    for title, artist in searches:
        if cancelled():
            return [], [], ""
        if time.monotonic() >= deadline:
            error = "Lyrics search timed out. Please try again."
            break
        params = {"track_name": title}
        if artist:
            params["artist_name"] = artist
        if track.album and (title, artist) == (track.title, track.artist):
            params["album_name"] = track.album
        request = Request(
            f"{API_URL}?{urlencode(params)}",
            headers={"User-Agent": USER_AGENT, "Accept": "application/json"},
        )
        results = []
        for attempt in range(MAX_REQUEST_ATTEMPTS):
            if cancelled():
                return [], [], ""
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                error = "Lyrics search timed out. Please try again."
                break
            retry = False
            try:
                with urlopen(request, timeout=min(REQUEST_TIMEOUT, remaining)) as response:
                    results = json.loads(response.read().decode("utf-8"))
                break
            except HTTPError as exc:
                exc.close()
                if exc.code == 404:
                    results = []
                    break
                if exc.code == 429:
                    error = "Lyrics service is rate limiting requests. Please wait a little and retry."
                else:
                    error = f"Lyrics service returned HTTP {exc.code}."
                    retry = exc.code in RETRYABLE_HTTP_CODES
            except (URLError, TimeoutError) as exc:
                error = f"Could not reach the lyrics service: {exc}"
                retry = True
            except json.JSONDecodeError as exc:
                error = f"Could not reach the lyrics service: {exc}"

            if cancelled():
                return [], [], ""
            if not retry or attempt == MAX_REQUEST_ATTEMPTS - 1 or have_lyrics():
                break
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                error = "Lyrics search timed out. Please try again."
                break
            delay = min(0.5 * (2 ** attempt), remaining)
            if cancel_event is None:
                time.sleep(delay)
            elif cancel_event.wait(delay):
                return [], [], ""
            error = ""
        if cancelled():
            return [], [], ""
        if error:
            break
        if isinstance(results, list):
            for record in results:
                if not isinstance(record, dict) or not _title_matches(
                    str(record.get("trackName", "")), title
                ):
                    continue
                # Search filters match keywords; validate the performer before
                # trusting timestamps, and keep artist hints tied to their song.
                record_artist = _artist_keys(str(record.get("artistName", "")))
                artist_matches = any(
                    record_artist & artist_keys
                    for candidate_title, artist_keys in known_artists
                    if _title_matches(str(record.get("trackName", "")), candidate_title)
                )
                record_id = (
                    str(record["id"]) if record.get("id") is not None
                    else json.dumps(record, sort_keys=True)
                )
                records[record_id] = (record, artist_matches)
            if any(
                artist_matches
                and _normalized(str(record.get("trackName", ""))) == _normalized(title)
                and (record.get("syncedLyrics") or record.get("plainLyrics"))
                and timing_compatible(record)
                for record, artist_matches in records.values()
            ):
                break
            if not artist and have_lyrics() and any(
                timing_compatible(record)
                and (record.get("syncedLyrics") or record.get("plainLyrics"))
                for record, _ in records.values()
            ):
                # All useful artist-filtered searches ran before title-only
                # fallbacks; don't delay readable lyrics with raw video metadata.
                break

    if error and not have_lyrics():
        return [], [], error
    if not records:
        return [], [], "Lyrics were not found."

    def score(record: dict, artist_matches: bool) -> int:
        points = 0
        if any(_normalized(str(record.get("trackName", ""))) == _normalized(title)
               for title, _ in searches):
            points += 2
        if artist_matches:
            points += 4
        return points

    def duration_score(record: dict) -> tuple[int, bool, float]:
        difference = duration_difference(record)
        if difference is not None:
            matches = difference <= 2.0
            # LRCLIB durations may be rounded. Keep useful timing when both
            # records already fit, instead of preferring an untimed duplicate.
            return 1 if matches else -1, matches and bool(record.get("syncedLyrics")), -difference
        # Missing duration is inconclusive, rather than a known wrong length.
        return 0, False, 0.0

    record, artist_matches = max(
        records.values(),
        key=lambda item: (
            bool(item[0].get("syncedLyrics") or item[0].get("plainLyrics")),
            score(*item),
            duration_score(item[0]),
            bool(track.album and str(item[0].get("albumName", "")).casefold() == track.album.casefold()),
            bool(item[0].get("syncedLyrics")),
        ),
    )
    synced = parse_lrc(str(record.get("syncedLyrics") or ""))
    plain_text = str(record.get("plainLyrics") or "").strip()
    plain = plain_text.splitlines()
    exact_title = any(
        _normalized(str(record.get("trackName", ""))) == _normalized(title)
        for title, _ in searches
    )
    if synced and artist_matches and exact_title:
        return synced, [], ""
    if synced and not plain:
        plain = "\n".join(line for _, line in synced).strip().splitlines()
    if plain:
        return [], plain, ""
    return [], [], "This match does not include readable lyrics."


def parse_lrc(text: str) -> list[tuple[float, str]]:
    lines: list[tuple[float, str]] = []
    for raw_line in text.splitlines():
        tags = list(TIME_TAG.finditer(raw_line))
        if not tags:
            continue
        lyric = TIME_TAG.sub("", raw_line).strip()
        for tag in tags:
            minutes, seconds = int(tag.group(1)), int(tag.group(2))
            fraction = tag.group(3)
            subsecond = int(fraction) / (10 ** len(fraction)) if fraction else 0.0
            lines.append((minutes * 60 + seconds + subsecond, lyric))
    return sorted(lines)


def active_line(lyrics: list[tuple[float, str]], position: float) -> int:
    current = -1
    for index, (timestamp, _) in enumerate(lyrics):
        if timestamp > position:
            break
        current = index
    return current
