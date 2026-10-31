import json
import unittest
from urllib.parse import parse_qs, urlsplit
from unittest.mock import patch

from terminal_lyrics.lyrics_service import fetch_lyrics, search_candidates
from terminal_lyrics.models import Track


COVER_TITLE = "Habits - Vintage 1930's Jazz Tove Lo Cover ft. Haley Reinhart"
COVER_TRACK = Track(COVER_TITLE, "PostmodernJukebox")


class JsonResponse:
    def __init__(self, records: list[dict]) -> None:
        self.records = records

    def __enter__(self):
        return self

    def __exit__(self, *args: object) -> None:
        return None

    def read(self) -> bytes:
        return json.dumps(self.records).encode("utf-8")


def query_params(request) -> dict[str, str]:
    return {
        key: values[0]
        for key, values in parse_qs(urlsplit(request.full_url).query, keep_blank_values=True).items()
    }


class CoverSearchTests(unittest.TestCase):
    def test_searches_title_before_cover_description_and_spaces_uploader(self) -> None:
        candidates = search_candidates(COVER_TRACK)

        self.assertIn(("Habits", "PostmodernJukebox"), candidates)
        self.assertIn(("Habits", "Postmodern Jukebox"), candidates)
        self.assertIn(("Habits", ""), candidates)

    def test_cover_description_preserves_artist_first_metadata_candidates(self) -> None:
        track = Track("Artist - Song - Acoustic Cover", "Uploader")

        self.assertIn(("Song", "Artist"), search_candidates(track))


class CoverLyricsTests(unittest.TestCase):
    def test_finds_synced_cover_using_spaced_uploader_name(self) -> None:
        record = {
            "id": 1,
            "trackName": "Habits",
            "artistName": "Postmodern Jukebox",
            "syncedLyrics": "[00:02.00]Cover line",
        }

        def respond(request, **kwargs):
            params = query_params(request)
            match = (
                params.get("track_name") == "Habits"
                and params.get("artist_name") == "Postmodern Jukebox"
            )
            return JsonResponse([record] if match else [])

        with patch("terminal_lyrics.lyrics_service.urlopen", side_effect=respond):
            result = fetch_lyrics(COVER_TRACK)

        self.assertEqual(result, ([(2.0, "Cover line")], [], ""))

    def test_derived_title_search_does_not_keep_unreliable_album_filter(self) -> None:
        record = {
            "id": 1,
            "trackName": "Habits",
            "artistName": "PostmodernJukebox",
            "syncedLyrics": "[00:02.00]Cover line",
        }
        queries: list[dict[str, str]] = []

        def respond(request, **kwargs):
            params = query_params(request)
            queries.append(params)
            match = (
                params.get("track_name") == "Habits"
                and params.get("artist_name") == "PostmodernJukebox"
                and not params.get("album_name")
            )
            return JsonResponse([record] if match else [])

        track = Track(COVER_TITLE, "PostmodernJukebox", "YouTube uploads")
        with patch("terminal_lyrics.lyrics_service.urlopen", side_effect=respond):
            result = fetch_lyrics(track)

        self.assertEqual(result, ([(2.0, "Cover line")], [], ""))
        self.assertTrue(
            any(query.get("track_name") == "Habits" and not query.get("album_name")
                for query in queries)
        )

    def test_title_only_original_artist_fallback_returns_plain_lyrics(self) -> None:
        record = {
            "id": 1,
            "trackName": "Habits",
            "artistName": "Tove Lo",
            "syncedLyrics": "[00:01.00]Original timing",
            "plainLyrics": "First line\nSecond line",
        }

        def respond(request, **kwargs):
            params = query_params(request)
            match = params.get("track_name") == "Habits" and not params.get("artist_name")
            return JsonResponse([record] if match else [])

        with patch("terminal_lyrics.lyrics_service.urlopen", side_effect=respond):
            result = fetch_lyrics(COVER_TRACK)

        self.assertEqual(result, ([], ["First line", "Second line"], ""))

    def test_original_artist_fallback_can_derive_plain_text_from_lrc(self) -> None:
        record = {
            "id": 1,
            "trackName": "Habits",
            "artistName": "Tove Lo",
            "syncedLyrics": "[00:01.00]First line\n[00:03.00]Second line",
        }

        def respond(request, **kwargs):
            params = query_params(request)
            match = params.get("track_name") == "Habits" and not params.get("artist_name")
            return JsonResponse([record] if match else [])

        with patch("terminal_lyrics.lyrics_service.urlopen", side_effect=respond):
            result = fetch_lyrics(COVER_TRACK)

        self.assertEqual(result, ([], ["First line", "Second line"], ""))

    def test_title_only_fallback_accepts_parenthetical_song_subtitle(self) -> None:
        record = {
            "id": 1,
            "trackName": "Habits (Stay High)",
            "artistName": "Tove Lo",
            "plainLyrics": "Original song line",
        }

        def respond(request, **kwargs):
            params = query_params(request)
            match = params.get("track_name") == "Habits" and not params.get("artist_name")
            return JsonResponse([record] if match else [])

        with patch("terminal_lyrics.lyrics_service.urlopen", side_effect=respond):
            result = fetch_lyrics(COVER_TRACK)

        self.assertEqual(result, ([], ["Original song line"], ""))

    def test_title_only_fallback_rejects_other_songs_containing_title(self) -> None:
        record = {
            "id": 1,
            "trackName": "Bad Habits",
            "artistName": "Ed Sheeran",
            "plainLyrics": "Unrelated song line",
        }

        def respond(request, **kwargs):
            params = query_params(request)
            match = params.get("track_name") == "Habits" and not params.get("artist_name")
            return JsonResponse([record] if match else [])

        with patch("terminal_lyrics.lyrics_service.urlopen", side_effect=respond):
            synced, plain, message = fetch_lyrics(COVER_TRACK)

        self.assertEqual(synced, [])
        self.assertEqual(plain, [])
        self.assertEqual(message, "Lyrics were not found.")

    def test_title_only_results_prefer_cover_over_original_recording(self) -> None:
        records = [
            {
                "id": 1,
                "trackName": "Habits",
                "artistName": "Tove Lo",
                "syncedLyrics": "[00:01.00]Original song line",
            },
            {
                "id": 2,
                "trackName": "Habits",
                "artistName": "Scott Bradlee's Postmodern Jukebox",
                "syncedLyrics": "[00:07.00]Cover line",
            },
        ]

        def respond(request, **kwargs):
            params = query_params(request)
            match = params.get("track_name") == "Habits" and not params.get("artist_name")
            return JsonResponse(records if match else [])

        with patch("terminal_lyrics.lyrics_service.urlopen", side_effect=respond):
            result = fetch_lyrics(COVER_TRACK)

        self.assertEqual(result, ([(7.0, "Cover line")], [], ""))

    def test_structured_artist_keyword_match_does_not_sync_tribute_recording(self) -> None:
        record = {
            "id": 1,
            "trackName": "Habits",
            "artistName": "PostmodernJukebox Tribute Band",
            "syncedLyrics": "[00:01.00]Tribute timing",
            "plainLyrics": "First line\nSecond line",
        }

        def respond(request, **kwargs):
            params = query_params(request)
            match = (
                params.get("track_name") == "Habits"
                and params.get("artist_name") == "PostmodernJukebox"
            )
            return JsonResponse([record] if match else [])

        with patch("terminal_lyrics.lyrics_service.urlopen", side_effect=respond):
            result = fetch_lyrics(COVER_TRACK)

        self.assertEqual(result, ([], ["First line", "Second line"], ""))

    def test_artist_hint_from_other_candidate_does_not_enable_sync(self) -> None:
        record = {
            "id": 1,
            "trackName": "Habits",
            "artistName": "Habits",
            "syncedLyrics": "[00:01.00]Different recording line",
        }

        def respond(request, **kwargs):
            params = query_params(request)
            match = params.get("track_name") == "Habits" and not params.get("artist_name")
            return JsonResponse([record] if match else [])

        with patch("terminal_lyrics.lyrics_service.urlopen", side_effect=respond):
            result = fetch_lyrics(COVER_TRACK)

        self.assertEqual(result, ([], ["Different recording line"], ""))

    def test_later_exact_song_match_beats_live_version_with_same_artist(self) -> None:
        live = {
            "id": 1,
            "trackName": "Song (Live)",
            "artistName": "Artist",
            "syncedLyrics": "[00:01.00]Live recording line",
        }
        exact = {
            "id": 2,
            "trackName": "Song",
            "artistName": "Artist",
            "syncedLyrics": "[00:03.00]Exact recording line",
        }

        def respond(request, **kwargs):
            params = query_params(request)
            if params.get("track_name") != "Song":
                return JsonResponse([])
            return JsonResponse([live] if params.get("artist_name") else [exact])

        with patch("terminal_lyrics.lyrics_service.urlopen", side_effect=respond):
            result = fetch_lyrics(Track("Song", "Artist"))

        self.assertEqual(result, ([(3.0, "Exact recording line")], [], ""))

    def test_records_without_ids_do_not_hide_readable_match(self) -> None:
        records = [
            {"trackName": "Song", "artistName": "Artist"},
            {
                "trackName": "Song",
                "artistName": "Artist",
                "syncedLyrics": "[00:01.00]Readable line",
            },
        ]

        with patch("terminal_lyrics.lyrics_service.urlopen", return_value=JsonResponse(records)):
            result = fetch_lyrics(Track("Song", "Artist"))

        self.assertEqual(result, ([(1.0, "Readable line")], [], ""))

    def test_regular_exact_match_retains_sync_and_stops_after_one_request(self) -> None:
        record = {
            "id": 1,
            "trackName": "Song",
            "artistName": "Artist",
            "syncedLyrics": "[00:01.00]First line",
        }

        with patch(
            "terminal_lyrics.lyrics_service.urlopen", return_value=JsonResponse([record])
        ) as request:
            result = fetch_lyrics(Track("Song", "Artist"))

        self.assertEqual(result, ([(1.0, "First line")], [], ""))
        self.assertEqual(request.call_count, 1)


if __name__ == "__main__":
    unittest.main()
