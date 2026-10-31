import unittest
from unittest.mock import Mock, patch

from terminal_lyrics.models import Track
from terminal_lyrics.ui import live


class Clock:
    def __init__(self) -> None:
        self.now = 100.0

    def monotonic(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.now += seconds


class TimedInput:
    def __init__(self, clock: Clock, duration: float, keys=()) -> None:
        self.clock = clock
        self.stop = clock.now + duration
        self.keys = iter(keys)

    def nodelay(self, enabled):
        pass

    def keypad(self, enabled):
        pass

    def getch(self):
        if self.clock.now >= self.stop:
            return ord("q")
        return next(self.keys, -1)


class LiveTimingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.clock = Clock()
        self.track = Track("Song", "Artist", player="brave", track_id="/track/1")
        self.loader = Mock()
        self.loader.poll.return_value = None
        self.patches = [
            patch("terminal_lyrics.ui.LyricsLoader", return_value=self.loader),
            patch("terminal_lyrics.ui.curses.curs_set"),
            patch("terminal_lyrics.ui.setup_theme"),
            patch("terminal_lyrics.ui.time.monotonic", side_effect=self.clock.monotonic),
            patch("terminal_lyrics.ui.time.sleep", side_effect=self.clock.sleep),
        ]
        for item in self.patches:
            item.start()
            self.addCleanup(item.stop)

    def test_active_lyric_updates_between_browser_position_polls(self) -> None:
        lyrics = [(0.0, "First line"), (0.3, "Next line")]
        result = (lyrics, [], "")
        delivered = False

        def poll(track):
            nonlocal delivered
            if delivered:
                return None
            delivered = True
            return result

        self.loader.poll.side_effect = poll
        window = TimedInput(self.clock, duration=0.8)
        with (
            patch("terminal_lyrics.ui.current_track", return_value=(self.track, 0.0, "Playing")) as player,
            patch("terminal_lyrics.ui.draw", return_value=(0, 0)) as draw,
        ):
            live(window, "brave", refresh=10.0)

        player.assert_called_once()
        positions = [frame.args[4] for frame in draw.call_args_list if frame.args[2] == lyrics]
        self.assertTrue(any(position is not None and position >= 0.3 for position in positions))
        self.assertLess(max(position for position in positions if position is not None), 1.0)
        self.loader.close.assert_called_once()

    def test_paused_position_does_not_advance_between_polls(self) -> None:
        window = TimedInput(self.clock, duration=1.2)
        with (
            patch("terminal_lyrics.ui.current_track", return_value=(self.track, 15.0, "Paused")) as player,
            patch("terminal_lyrics.ui.draw", return_value=(0, 0)) as draw,
        ):
            live(window, "brave", refresh=10.0)

        player.assert_called_once()
        self.assertTrue(draw.call_args_list)
        self.assertTrue(all(frame.args[4] == 15.0 for frame in draw.call_args_list))

    def test_unknown_browser_position_is_not_drawn_as_zero(self) -> None:
        window = TimedInput(self.clock, duration=0.2)
        with (
            patch("terminal_lyrics.ui.current_track", return_value=(self.track, None, "Playing")),
            patch("terminal_lyrics.ui.draw", return_value=(0, 0)) as draw,
        ):
            live(window, "brave", refresh=10.0)

        self.assertTrue(draw.call_args_list)
        self.assertTrue(all(frame.args[4] is None for frame in draw.call_args_list))

    def test_keyboard_timing_adjustment_changes_offset_without_moving_player(self) -> None:
        window = TimedInput(self.clock, duration=0.25, keys=[ord("+"), ord("-"), ord("="), ord("0")])
        with (
            patch("terminal_lyrics.ui.current_track", return_value=(self.track, 10.0, "Playing")),
            patch("terminal_lyrics.ui.draw", return_value=(0, 0)) as draw,
            patch("terminal_lyrics.ui.run_playerctl") as control,
        ):
            live(window, "brave", refresh=10.0)

        offsets = [frame.kwargs.get("lyric_offset", 0.0) for frame in draw.call_args_list]
        self.assertIn(0.25, offsets)
        self.assertEqual(offsets[-1], 0.0)
        control.assert_not_called()


if __name__ == "__main__":
    unittest.main()
