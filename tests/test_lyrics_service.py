import unittest
from urllib.error import HTTPError
from unittest.mock import patch

from terminal_lyrics.lyrics_service import active_line, fetch_lyrics, parse_lrc, search_candidates
from terminal_lyrics.models import Track


class ParseLrcTests(unittest.TestCase):
    def test_parses_and_sorts_timestamped_lines(self) -> None:
        self.assertEqual(
            parse_lrc("[00:02.5]Later\n[00:01.25][00:03.00]Repeated"),
            [(1.25, "Repeated"), (2.5, "Later"), (3.0, "Repeated")],
        )

    def test_ignores_lines_without_timestamps(self) -> None:
        self.assertEqual(parse_lrc("metadata\njust text"), [])

    def test_active_line_tracks_latest_timestamp(self) -> None:
        lyrics = [(1.0, "first"), (3.0, "second")]
        self.assertEqual(active_line(lyrics, 2.0), 0)
        self.assertEqual(active_line(lyrics, 3.0), 1)


class SearchCandidateTests(unittest.TestCase):
    def test_extracts_title_from_video_style_metadata(self) -> None:
        track = Track(
            "She & him - I Thought I Saw Your Face Today (Official Lyric Video) – SheandHimOfficial",
            "",
        )
        candidates = search_candidates(track)
        self.assertIn(("I Thought I Saw Your Face Today", "She & him"), candidates)

    def test_extracts_song_and_artist_from_double_slash_title(self) -> None:
        track = Track(
            "Mac DeMarco // No Other Heart (Official Audio) – CapturedTracks",
            "",
        )
        candidates = search_candidates(track)
        self.assertIn(("No Other Heart", "Mac DeMarco"), candidates)


class FetchLyricsTests(unittest.TestCase):
    def test_returns_synced_lyrics_for_matching_record(self) -> None:
        record = {
            "id": 1,
            "trackName": "Song",
            "artistName": "Artist",
            "syncedLyrics": "[00:01.00]First line",
            "plainLyrics": "First line",
        }

        class Response:
            def __enter__(self):
                return self

            def __exit__(self, *args: object) -> None:
                _ = args
                return None

            def read(self) -> bytes:
                import json

                return json.dumps([record]).encode("utf-8")

        with patch("terminal_lyrics.lyrics_service.urlopen", return_value=Response()):
            synced, plain, message = fetch_lyrics(Track("Song", "Artist"))

        self.assertEqual(synced, [(1.0, "First line")])
        self.assertEqual(plain, [])
        self.assertEqual(message, "")

    def test_retries_temporary_service_unavailable_response(self) -> None:
        record = {
            "id": 1,
            "trackName": "Song",
            "artistName": "Artist",
            "syncedLyrics": "[00:01.00]First line",
        }

        class Response:
            def __enter__(self):
                return self

            def __exit__(self, *args: object) -> None:
                _ = args
                return None

            def read(self) -> bytes:
                import json

                return json.dumps([record]).encode("utf-8")

        unavailable = HTTPError("https://lrclib.net/api/search", 503, "Unavailable", None, None)
        with (
            patch(
                "terminal_lyrics.lyrics_service.urlopen",
                side_effect=[unavailable, Response()],
            ) as request,
            patch("terminal_lyrics.lyrics_service.time.sleep") as sleep,
        ):
            synced, plain, message = fetch_lyrics(Track("Song", "Artist"))

        self.assertEqual(request.call_count, 2)
        sleep.assert_called_once_with(0.5)
        self.assertEqual(synced, [(1.0, "First line")])
        self.assertEqual(plain, [])
        self.assertEqual(message, "")

    def test_prefers_lyric_result_over_empty_duplicate_match(self) -> None:
        records = [
            {"id": 1, "trackName": "Song", "artistName": "Artist"},
            {
                "id": 2,
                "trackName": "Song",
                "artistName": "Artist",
                "syncedLyrics": "[00:01.00]First line",
            },
        ]

        class Response:
            def __enter__(self):
                return self

            def __exit__(self, *args: object) -> None:
                _ = args
                return None

            def read(self) -> bytes:
                import json

                return json.dumps(records).encode("utf-8")

        with patch("terminal_lyrics.lyrics_service.urlopen", return_value=Response()):
            synced, plain, message = fetch_lyrics(Track("Song", "Artist"))

        self.assertEqual(synced, [(1.0, "First line")])
        self.assertEqual(plain, [])
        self.assertEqual(message, "")


if __name__ == "__main__":
    unittest.main()
