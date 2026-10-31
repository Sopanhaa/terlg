import unittest
from unittest.mock import Mock, patch

from terminal_lyrics.models import Track
from terminal_lyrics.ui import live


class InputWindow:
    def __init__(self, keys):
        self.keys = iter(keys)

    def nodelay(self, enabled):
        pass

    def keypad(self, enabled):
        pass

    def getch(self):
        return next(self.keys, ord("q"))


def frame_message(frame):
    return frame.kwargs.get("message", frame.args[6] if len(frame.args) > 6 else "")


class LivePerformanceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.track = Track("Song", "Artist", player="brave")
        self.loader = Mock()
        self.loader.poll.return_value = None
        self.patches = [
            patch("terminal_lyrics.ui.LyricsLoader", return_value=self.loader),
            patch("terminal_lyrics.ui.curses.curs_set"),
            patch("terminal_lyrics.ui.setup_theme"),
            patch("terminal_lyrics.ui.current_track", return_value=(self.track, 0.0, "Playing")),
            patch("terminal_lyrics.ui.time.sleep"),
        ]
        self.started = [item.start() for item in self.patches]
        for item in reversed(self.patches):
            self.addCleanup(item.stop)

    def test_pending_lookup_draws_loading_and_keeps_controls_and_quit_responsive(self) -> None:
        window = InputWindow([-1, ord(" "), ord("\t"), ord("q")])

        with (
            patch("terminal_lyrics.ui.draw", return_value=(0, 0)) as draw,
            patch("terminal_lyrics.ui.run_playerctl") as control,
        ):
            live(window, "brave", refresh=10.0)

        self.loader.request.assert_called_once_with(self.track)
        self.assertTrue(any("Loading lyrics" in frame_message(frame) for frame in draw.call_args_list))
        self.assertTrue(any(frame.kwargs.get("view") == "overview" for frame in draw.call_args_list))
        control.assert_called_once_with("play-pause", player="brave")
        self.loader.close.assert_called_once()
        sleeps = self.started[-1]
        self.assertTrue(all(call.args[0] <= 0.05 for call in sleeps.call_args_list))

    def test_completed_lyrics_are_displayed_without_waiting_for_player_poll(self) -> None:
        window = InputWindow([-1, -1, ord("q")])
        result = ([(1.0, "Loaded lyric")], [], "")

        with patch("terminal_lyrics.ui.draw", return_value=(0, 0)) as draw:
            self.loader.poll.side_effect = lambda track: result if draw.call_count else None
            live(window, "brave", refresh=10.0)

        self.assertTrue(any("Loading lyrics" in frame_message(frame) for frame in draw.call_args_list))
        self.assertTrue(any(frame.args[2] == result[0] and not frame_message(frame) for frame in draw.call_args_list))
        self.loader.close.assert_called_once()

    def test_worker_is_closed_when_rendering_raises(self) -> None:
        window = InputWindow([-1])

        with patch("terminal_lyrics.ui.draw", side_effect=RuntimeError("render failed")):
            with self.assertRaisesRegex(RuntimeError, "render failed"):
                live(window, "brave", refresh=0.5)

        self.loader.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()
