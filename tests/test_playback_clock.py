import unittest

from terminal_lyrics.models import Track
from terminal_lyrics.playback_clock import PlaybackClock


class PlaybackClockTests(unittest.TestCase):
    def setUp(self) -> None:
        self.track = Track("Song", "Artist", player="brave", track_id="/track/1")
        self.clock = PlaybackClock()

    def test_playing_position_advances_between_player_samples(self) -> None:
        self.clock.update(self.track, 10.0, "Playing", now=100.0)

        self.assertAlmostEqual(self.clock.position(now=102.75), 12.75)

    def test_paused_and_stopped_samples_do_not_advance(self) -> None:
        for status in ("Paused", "Stopped"):
            with self.subTest(status=status):
                self.clock.update(self.track, 15.0, status, now=100.0)
                self.assertEqual(self.clock.position(now=120.0), 15.0)

    def test_missing_position_does_not_invent_a_start_time(self) -> None:
        self.clock.update(self.track, None, "Playing", now=100.0)

        self.assertIsNone(self.clock.position(now=110.0))

    def test_missing_sample_keeps_the_known_playback_timeline(self) -> None:
        self.clock.update(self.track, 10.0, "Playing", now=100.0)
        self.clock.update(self.track, None, "Playing", now=105.0)

        self.assertEqual(self.clock.position(now=107.0), 17.0)

    def test_pause_and_resume_without_position_keep_the_estimated_time(self) -> None:
        self.clock.update(self.track, 10.0, "Playing", now=100.0)
        self.clock.update(self.track, None, "Paused", now=105.0)
        self.assertEqual(self.clock.position(now=110.0), 15.0)

        self.clock.update(self.track, None, "Playing", now=110.0)

        self.assertEqual(self.clock.position(now=112.0), 17.0)

    def test_authoritative_backward_and_forward_seeks_reset_the_clock(self) -> None:
        self.clock.update(self.track, 30.0, "Playing", now=100.0)
        self.clock.update(self.track, 2.0, "Playing", now=105.0)
        self.assertEqual(self.clock.position(now=106.0), 3.0)

        self.clock.update(self.track, 60.0, "Playing", now=107.0)

        self.assertEqual(self.clock.position(now=108.0), 61.0)

    def test_repeated_valid_sample_remains_authoritative(self) -> None:
        self.clock.update(self.track, 10.0, "Playing", now=100.0)
        self.clock.update(self.track, 10.0, "Playing", now=101.0)

        self.assertEqual(self.clock.position(now=102.0), 11.0)

    def test_new_song_player_or_track_id_resets_unavailable_timing(self) -> None:
        changes = [
            Track("Other Song", "Artist", player="brave", track_id="/track/1"),
            Track("Song", "Artist", player="vlc", track_id="/track/1"),
            Track("Song", "Artist", player="brave", track_id="/track/2"),
        ]
        for track in changes:
            with self.subTest(track=track):
                self.clock.update(self.track, 10.0, "Playing", now=100.0)
                self.clock.update(track, None, "Playing", now=105.0)
                self.assertIsNone(self.clock.position(now=110.0))

    def test_position_stops_at_the_known_track_duration(self) -> None:
        track = Track("Song", "Artist", player="brave", duration=12.0)
        self.clock.update(track, 10.0, "Playing", now=100.0)

        self.assertEqual(self.clock.position(now=110.0), 12.0)

    def test_clock_read_before_anchor_does_not_produce_negative_elapsed_time(self) -> None:
        self.clock.update(self.track, 2.0, "Playing", now=100.0)

        self.assertEqual(self.clock.position(now=99.0), 2.0)


if __name__ == "__main__":
    unittest.main()
