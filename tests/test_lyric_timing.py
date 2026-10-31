import json
import unittest
from unittest.mock import patch

from terminal_lyrics.lyrics_service import active_line, fetch_lyrics, parse_lrc
from terminal_lyrics.models import Track


class JsonResponse:
    def __init__(self, records: list[dict]) -> None:
        self.records = records

    def __enter__(self):
        return self

    def __exit__(self, *args: object) -> None:
        pass

    def read(self) -> bytes:
        return json.dumps(self.records).encode("utf-8")


class LyricTimingTests(unittest.TestCase):
    def test_preserves_three_digit_timestamp_precision(self) -> None:
        self.assertEqual(
            parse_lrc("[00:01.001]First\n[00:01.009]Second\n[00:01.999]Third"),
            [(1.001, "First"), (1.009, "Second"), (1.999, "Third")],
        )

    def test_no_line_is_active_before_first_timestamp(self) -> None:
        lyrics = [(5.0, "First"), (6.0, "Second")]
        self.assertEqual(active_line(lyrics, 4.999), -1)
        self.assertEqual(active_line(lyrics, 5.0), 0)
        self.assertEqual(active_line([], 100.0), -1)

    def test_matching_duration_beats_different_arrangement_and_album(self) -> None:
        records = [
            {
                "id": 1, "trackName": "Song", "artistName": "Artist",
                "albumName": "Video album", "duration": 240,
                "syncedLyrics": "[00:10.00]Different arrangement",
            },
            {
                "id": 2, "trackName": "Song", "artistName": "Artist",
                "albumName": "Original album", "duration": 180.5,
                "syncedLyrics": "[00:02.00]Matching arrangement",
            },
        ]

        with patch("terminal_lyrics.lyrics_service.urlopen", return_value=JsonResponse(records)) as request:
            result = fetch_lyrics(Track("Song", "Artist", "Video album", duration=180))

        self.assertEqual(result, ([(2.0, "Matching arrangement")], [], ""))
        self.assertEqual(request.call_count, 1)

    def test_duration_mismatch_does_not_stop_existing_fallback_search(self) -> None:
        wrong = {
            "id": 1, "trackName": "Song", "artistName": "Artist", "duration": 240,
            "syncedLyrics": "[00:10.00]Different arrangement",
        }
        right = {
            "id": 2, "trackName": "Song", "artistName": "Artist", "duration": 180,
            "syncedLyrics": "[00:02.00]Matching arrangement",
        }

        with patch(
            "terminal_lyrics.lyrics_service.urlopen",
            side_effect=[JsonResponse([wrong]), JsonResponse([right])],
        ) as request:
            result = fetch_lyrics(Track("Song", "Artist", duration=180))

        self.assertEqual(result, ([(2.0, "Matching arrangement")], [], ""))
        self.assertEqual(request.call_count, 2)

    def test_synced_record_within_duration_tolerance_beats_plain_duplicate(self) -> None:
        records = [
            {
                "id": 1, "trackName": "Song", "artistName": "Artist", "duration": 180,
                "plainLyrics": "Untimed duplicate",
            },
            {
                "id": 2, "trackName": "Song", "artistName": "Artist", "duration": 181,
                "syncedLyrics": "[00:02.00]Matching arrangement",
            },
        ]

        with patch("terminal_lyrics.lyrics_service.urlopen", return_value=JsonResponse(records)):
            result = fetch_lyrics(Track("Song", "Artist", duration=180))

        self.assertEqual(result, ([(2.0, "Matching arrangement")], [], ""))

    def test_duration_does_not_override_performer_identity(self) -> None:
        records = [
            {
                "id": 1, "trackName": "Song", "artistName": "Other Artist", "duration": 180,
                "syncedLyrics": "[00:01.00]Other performer",
            },
            {
                "id": 2, "trackName": "Song", "artistName": "Artist", "duration": 181,
                "syncedLyrics": "[00:03.00]Expected performer",
            },
        ]

        with patch("terminal_lyrics.lyrics_service.urlopen", return_value=JsonResponse(records)):
            result = fetch_lyrics(Track("Song", "Artist", duration=180))

        self.assertEqual(result, ([(3.0, "Expected performer")], [], ""))

    def test_unknown_duration_beats_a_known_different_length(self) -> None:
        records = [
            {
                "id": 1, "trackName": "Song", "artistName": "Artist", "duration": 240,
                "syncedLyrics": "[00:10.00]Different arrangement",
            },
            {
                "id": 2, "trackName": "Song", "artistName": "Artist",
                "syncedLyrics": "[00:02.00]Unspecified duration",
            },
        ]
        with patch("terminal_lyrics.lyrics_service.urlopen", return_value=JsonResponse(records)):
            result = fetch_lyrics(Track("Song", "Artist", duration=180))

        self.assertEqual(result, ([(2.0, "Unspecified duration")], [], ""))

    def test_unavailable_record_duration_preserves_normal_exact_lookup(self) -> None:
        for duration in (None, "unknown", "nan", "inf", -1):
            record = {
                "id": 1, "trackName": "Song", "artistName": "Artist", "duration": duration,
                "syncedLyrics": "[00:01.00]Expected line",
            }
            with self.subTest(duration=duration), patch(
                "terminal_lyrics.lyrics_service.urlopen", return_value=JsonResponse([record])
            ) as request:
                result = fetch_lyrics(Track("Song", "Artist", duration=180))
            self.assertEqual(result, ([(1.0, "Expected line")], [], ""))
            self.assertEqual(request.call_count, 1)


if __name__ == "__main__":
    unittest.main()
