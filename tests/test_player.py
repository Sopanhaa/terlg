import subprocess
import unittest
from unittest.mock import patch

from terminal_lyrics.models import Track
from terminal_lyrics.player import METADATA_FORMAT, METADATA_SEPARATOR, current_track, run_playerctl


def snapshot(**values: str) -> str:
    fields = {
        "title": "Song", "artist": "Artist", "album": "Album",
        "player": "brave", "status": "Playing", "position": "12345678",
        "duration": "240000000", "track_id": "/org/mpris/MediaPlayer2/track/1",
    }
    fields.update(values)
    return METADATA_SEPARATOR.join(fields.values())


class PlayerSnapshotTests(unittest.TestCase):
    def test_reads_track_status_and_microsecond_position_in_one_call(self) -> None:
        with patch("terminal_lyrics.player.run_playerctl", return_value=snapshot()) as command:
            track, position, status = current_track("brave")

        command.assert_called_once_with("metadata", "--format", METADATA_FORMAT, player="brave")
        self.assertEqual(track, Track("Song", "Artist", "Album", "brave", 240.0,
                                      "/org/mpris/MediaPlayer2/track/1"))
        self.assertAlmostEqual(position, 12.345678)
        self.assertEqual(status, "Playing")

    def test_empty_artist_and_album_are_valid_metadata(self) -> None:
        with patch("terminal_lyrics.player.run_playerctl", return_value=snapshot(artist="", album="")):
            track, position, _ = current_track(None)

        self.assertEqual((track.artist, track.album), ("", ""))
        self.assertAlmostEqual(position, 12.345678)

    def test_missing_title_is_not_replaced_by_the_artist(self) -> None:
        output = snapshot(title="") + "\n"
        result = subprocess.CompletedProcess(["playerctl"], 0, output, "")
        with patch("terminal_lyrics.player.subprocess.run", return_value=result):
            with self.assertRaisesRegex(RuntimeError, "No song"):
                current_track("brave")

    def test_unavailable_or_invalid_position_remains_unknown(self) -> None:
        for value in ("", "unknown", "nan", "inf", "-inf", "-1000000"):
            with self.subTest(position=value), patch(
                "terminal_lyrics.player.run_playerctl", return_value=snapshot(position=value)
            ):
                _, position, _ = current_track("brave")
                self.assertIsNone(position)

    def test_zero_position_is_distinct_from_unavailable_timing(self) -> None:
        with patch("terminal_lyrics.player.run_playerctl", return_value=snapshot(position="0")):
            _, position, _ = current_track("brave")

        self.assertEqual(position, 0.0)

    def test_unknown_duration_and_track_id_keep_default_values(self) -> None:
        with patch("terminal_lyrics.player.run_playerctl", return_value=snapshot(duration="", track_id="")):
            track, _, _ = current_track("brave")

        self.assertEqual((track.duration, track.track_id), (0.0, ""))

    def test_invalid_duration_is_not_exposed_as_nonfinite_or_negative(self) -> None:
        for value in ("nan", "inf", "-1000000", "unknown"):
            with self.subTest(duration=value), patch(
                "terminal_lyrics.player.run_playerctl", return_value=snapshot(duration=value)
            ):
                track, _, _ = current_track("brave")
                self.assertEqual(track.duration, 0.0)

    def test_player_name_can_fall_back_to_explicit_selection(self) -> None:
        with patch("terminal_lyrics.player.run_playerctl", return_value=snapshot(player="")):
            track, _, _ = current_track("brave.instance123")

        self.assertEqual(track.player, "brave.instance123")

    def test_multiline_title_does_not_shift_the_timing_fields(self) -> None:
        with patch("terminal_lyrics.player.run_playerctl", return_value=snapshot(title="First\nSecond")):
            track, position, status = current_track("brave")

        self.assertEqual(track.title, "First\nSecond")
        self.assertAlmostEqual(position, 12.345678)
        self.assertEqual(status, "Playing")

    def test_incomplete_snapshot_is_reported(self) -> None:
        with patch("terminal_lyrics.player.run_playerctl", return_value="Song\x1fArtist"):
            with self.assertRaisesRegex(RuntimeError, "metadata"):
                current_track("brave")

    def test_timing_metadata_does_not_change_the_lyrics_cache_key(self) -> None:
        first = Track("Song", "Artist", "Album", "brave", 200.0, "/track/1")
        second = Track("SONG", "ARTIST", "ALBUM", "vlc", 201.0, "/track/2")

        self.assertEqual(first.key, second.key)


class PlayerCommandTests(unittest.TestCase):
    def test_snapshot_keeps_trailing_empty_fields(self) -> None:
        output = snapshot(position="", duration="", track_id="")
        result = subprocess.CompletedProcess(["playerctl"], 0, output + "\n", "")
        with patch("terminal_lyrics.player.subprocess.run", return_value=result):
            self.assertEqual(run_playerctl("metadata", "--format", METADATA_FORMAT), output)


if __name__ == "__main__":
    unittest.main()
