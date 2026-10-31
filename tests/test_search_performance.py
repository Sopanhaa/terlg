import json
from threading import Event
import unittest
from urllib.error import HTTPError, URLError
from urllib.parse import parse_qs, urlsplit
from unittest.mock import patch

from terminal_lyrics.lyrics_service import fetch_lyrics, search_candidates
from terminal_lyrics.models import Track


COVER_TRACK = Track(
    "Habits - Vintage 1930's Jazz Tove Lo Cover ft. Haley Reinhart",
    "PostmodernJukebox",
)
FALLBACK = {
    "id": 1,
    "trackName": "Habits",
    "artistName": "Tove Lo",
    "plainLyrics": "First verse\n\nSecond verse",
}


class JsonResponse:
    def __init__(self, records: list[dict], on_read=None) -> None:
        self.records = records
        self.on_read = on_read

    def __enter__(self):
        return self

    def __exit__(self, *args: object) -> None:
        pass

    def read(self) -> bytes:
        if self.on_read is not None:
            self.on_read()
        return json.dumps(self.records).encode("utf-8")


class Clock:
    def __init__(self) -> None:
        self.now = 0.0

    def monotonic(self) -> float:
        return self.now

    def sleep(self, delay: float) -> None:
        self.now += delay


class SearchPerformanceTests(unittest.TestCase):
    def test_cover_search_prioritizes_song_and_limits_noisy_queries(self) -> None:
        candidates = search_candidates(COVER_TRACK)

        self.assertEqual(candidates[:3], [
            ("Habits", "PostmodernJukebox"),
            ("Habits", "Postmodern Jukebox"),
            ("Habits", ""),
        ])
        self.assertLessEqual(len(candidates), 4)
        self.assertFalse(any(title.startswith("Vintage") for title, _ in candidates))

    def test_inferred_performer_and_song_are_searched_before_video_metadata(self) -> None:
        track = Track("Artist // Song (Official Audio) - Uploader", "Channel")

        self.assertEqual(search_candidates(track)[0], ("Song", "Artist"))

    def test_ordinary_dashed_title_keeps_exact_song_ahead_of_inferred_fragments(self) -> None:
        track = Track("Song - Part Two", "Artist")
        queries = []

        def respond(request, **kwargs):
            params = parse_qs(urlsplit(request.full_url).query)
            title = params["track_name"][0]
            artist = params.get("artist_name", [""])[0]
            queries.append((title, artist))
            if artist != "Artist":
                return JsonResponse([])
            if title == track.title:
                return JsonResponse([{
                    "id": 1, "trackName": track.title, "artistName": "Artist",
                    "syncedLyrics": "[00:01.00]Correct recording",
                }])
            if title == "Part Two":
                return JsonResponse([{
                    "id": 2, "trackName": "Part Two", "artistName": "Artist",
                    "syncedLyrics": "[00:02.00]Different song",
                }])
            return JsonResponse([])

        with patch("terminal_lyrics.lyrics_service.urlopen", side_effect=respond):
            result = fetch_lyrics(track)

        self.assertEqual(result, ([(1.0, "Correct recording")], [], ""))
        self.assertEqual(queries, [(track.title, track.artist)])

    def test_spaced_cover_artist_is_found_in_two_requests(self) -> None:
        record = {
            "id": 2, "trackName": "Habits", "artistName": "Postmodern Jukebox",
            "syncedLyrics": "[00:01.00]Cover line",
        }

        with patch(
            "terminal_lyrics.lyrics_service.urlopen",
            side_effect=[JsonResponse([]), JsonResponse([record])],
        ) as request:
            result = fetch_lyrics(COVER_TRACK)

        self.assertEqual(result, ([(1.0, "Cover line")], [], ""))
        self.assertEqual(request.call_count, 2)

    def test_title_only_fallback_does_not_wait_for_raw_video_query(self) -> None:
        queries = []

        def respond(request, **kwargs):
            params = parse_qs(urlsplit(request.full_url).query)
            queries.append(params)
            return JsonResponse([FALLBACK] if not params.get("artist_name") else [])

        with patch("terminal_lyrics.lyrics_service.urlopen", side_effect=respond):
            result = fetch_lyrics(COVER_TRACK)

        self.assertEqual(result, ([], ["First verse", "", "Second verse"], ""))
        self.assertEqual(len(queries), 3)
        self.assertTrue(all(query["track_name"] == ["Habits"] for query in queries))

    def test_cancellation_before_search_performs_no_requests(self) -> None:
        cancelled = Event()
        cancelled.set()

        with patch("terminal_lyrics.lyrics_service.urlopen") as request:
            result = fetch_lyrics(COVER_TRACK, cancel_event=cancelled)

        self.assertEqual(result, ([], [], ""))
        request.assert_not_called()

    def test_cancellation_during_response_skips_all_fallback_requests(self) -> None:
        cancelled = Event()
        with patch(
            "terminal_lyrics.lyrics_service.urlopen",
            return_value=JsonResponse([], on_read=cancelled.set),
        ) as request:
            result = fetch_lyrics(COVER_TRACK, cancel_event=cancelled)

        self.assertEqual(result, ([], [], ""))
        self.assertEqual(request.call_count, 1)

    def test_cancellation_interrupts_retry_backoff(self) -> None:
        class CancelOnWait(Event):
            def wait(self, timeout=None):
                self.set()
                return True

        cancelled = CancelOnWait()
        unavailable = HTTPError("https://lrclib.net/api/search", 503, "Unavailable", None, None)
        with (
            patch("terminal_lyrics.lyrics_service.urlopen", side_effect=unavailable) as request,
            patch("terminal_lyrics.lyrics_service.time.sleep") as sleep,
        ):
            result = fetch_lyrics(COVER_TRACK, cancel_event=cancelled)

        self.assertEqual(result, ([], [], ""))
        self.assertEqual(request.call_count, 1)
        sleep.assert_not_called()

    def test_search_budget_caps_request_timeout_and_stops_fallbacks(self) -> None:
        clock = Clock()
        timeouts = []

        def respond(request, timeout):
            timeouts.append(timeout)
            clock.now += 3.5 if len(timeouts) == 1 else timeout
            return JsonResponse([])

        with (
            patch("terminal_lyrics.lyrics_service.SEARCH_TIMEOUT", 5.0),
            patch("terminal_lyrics.lyrics_service.time.monotonic", side_effect=clock.monotonic),
            patch("terminal_lyrics.lyrics_service.urlopen", side_effect=respond),
        ):
            synced, plain, message = fetch_lyrics(COVER_TRACK)

        self.assertEqual(timeouts, [4.0, 1.5])
        self.assertEqual((synced, plain), ([], []))
        self.assertIn("timed out", message)

    def test_retry_sleep_cannot_exceed_remaining_search_budget(self) -> None:
        clock = Clock()
        unavailable = HTTPError("https://lrclib.net/api/search", 503, "Unavailable", None, None)
        with (
            patch("terminal_lyrics.lyrics_service.SEARCH_TIMEOUT", 0.2),
            patch("terminal_lyrics.lyrics_service.time.monotonic", side_effect=clock.monotonic),
            patch("terminal_lyrics.lyrics_service.time.sleep", side_effect=clock.sleep) as sleep,
            patch("terminal_lyrics.lyrics_service.urlopen", side_effect=unavailable) as request,
        ):
            _, _, message = fetch_lyrics(COVER_TRACK)

        sleep.assert_called_once_with(0.2)
        self.assertEqual(request.call_count, 1)
        self.assertEqual(request.call_args.kwargs["timeout"], 0.2)
        self.assertIn("timed out", message)

    def test_readable_fallback_survives_later_optional_request_error(self) -> None:
        for error in (
            HTTPError("https://lrclib.net/api/search", 503, "Unavailable", None, None),
            HTTPError("https://lrclib.net/api/search", 429, "Rate limited", None, None),
            URLError("Connection lost"),
        ):
            with self.subTest(error=error), patch(
                "terminal_lyrics.lyrics_service.urlopen",
                side_effect=[JsonResponse([FALLBACK]), error],
            ) as request:
                result = fetch_lyrics(COVER_TRACK)

            self.assertEqual(result, ([], ["First verse", "", "Second verse"], ""))
            self.assertEqual(request.call_count, 2)

    def test_readable_fallback_survives_exhausted_search_budget(self) -> None:
        clock = Clock()

        def use_budget():
            clock.now = 5.0

        with (
            patch("terminal_lyrics.lyrics_service.SEARCH_TIMEOUT", 5.0),
            patch("terminal_lyrics.lyrics_service.time.monotonic", side_effect=clock.monotonic),
            patch(
                "terminal_lyrics.lyrics_service.urlopen",
                return_value=JsonResponse([FALLBACK], on_read=use_budget),
            ) as request,
        ):
            result = fetch_lyrics(COVER_TRACK)

        self.assertEqual(result, ([], ["First verse", "", "Second verse"], ""))
        self.assertEqual(request.call_count, 1)


if __name__ == "__main__":
    unittest.main()
