import contextlib
import curses
import io
import unittest
from unittest.mock import patch

from terminal_lyrics import cli
from terminal_lyrics.models import Track
from terminal_lyrics.ui import draw
from test_ui_rendering import FakeWindow


class DrawTimingControlsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.track = Track("Song", "Artist", player="brave")
        self.lyrics = [(0.0, "FIRST-CUE"), (5.0, "SECOND-CUE"), (10.0, "THIRD-CUE")]
        self.theme = patch("terminal_lyrics.ui._theme_pairs", set())
        self.theme.start()
        self.addCleanup(self.theme.stop)

    def highlighted(self, window: FakeWindow) -> set[str]:
        return {
            cue for _, cue in self.lyrics
            if any(cue in text and attr & curses.A_REVERSE for _, _, text, attr in window.writes)
        }

    def test_positive_adjustment_advances_highlight_without_changing_playback_time(self) -> None:
        window = FakeWindow()

        draw(window, self.track, self.lyrics, [], 4.5, "Playing", lyric_offset=1.0)

        self.assertEqual(self.highlighted(window), {"SECOND-CUE"})
        self.assertIn("00:04", window.text)

    def test_negative_adjustment_delays_highlight(self) -> None:
        window = FakeWindow()

        draw(window, self.track, self.lyrics, [], 5.5, "Playing", lyric_offset=-1.0)

        self.assertEqual(self.highlighted(window), {"FIRST-CUE"})
        self.assertIn("00:05", window.text)

    def test_negative_adjustment_can_delay_a_cue_at_zero(self) -> None:
        window = FakeWindow()

        draw(window, self.track, self.lyrics, [], 0.0, "Playing", lyric_offset=-0.25)
        self.assertEqual(self.highlighted(window), set())

        draw(window, self.track, self.lyrics, [], 0.25, "Playing", lyric_offset=-0.25)
        self.assertEqual(self.highlighted(window), {"FIRST-CUE"})

    def test_unknown_playback_time_is_not_created_by_an_adjustment(self) -> None:
        window = FakeWindow()

        draw(window, self.track, self.lyrics, [], None, "Playing", lyric_offset=2.0)

        self.assertEqual(self.highlighted(window), set())
        self.assertIn("--:--", window.text)
        self.assertIn("Playback time unavailable", window.text)

    def test_adjustment_does_not_fabricate_timing_for_plain_lyrics(self) -> None:
        window = FakeWindow()
        plain = [cue for _, cue in self.lyrics]

        draw(window, self.track, [], plain, 4.5, "Playing", lyric_offset=2.0)

        self.assertEqual(self.highlighted(window), set())
        self.assertIn("Plain lyrics", window.text)


class CliTimingControlsTests(unittest.TestCase):
    def test_offset_is_passed_to_the_curses_application(self) -> None:
        with (
            patch("sys.argv", ["lyrics.py", "--player", "brave", "--refresh", "0.2", "--offset", "1.5"]),
            patch("terminal_lyrics.cli.shutil.which", return_value="/usr/bin/playerctl"),
            patch("terminal_lyrics.cli.curses.wrapper") as wrapper,
        ):
            self.assertEqual(cli.main(), 0)

        wrapper.assert_called_once_with(cli.live, "brave", 0.2, 1.5)

    def test_nonfinite_offset_is_rejected_before_starting_the_application(self) -> None:
        for value in ("nan", "inf", "-inf"):
            with (
                self.subTest(offset=value),
                patch("sys.argv", ["lyrics.py", f"--offset={value}"]),
                patch("terminal_lyrics.cli.shutil.which") as executable,
                patch("terminal_lyrics.cli.curses.wrapper") as wrapper,
                contextlib.redirect_stderr(io.StringIO()) as stderr,
            ):
                with self.assertRaises(SystemExit) as exited:
                    cli.main()

            self.assertEqual(exited.exception.code, 2)
            self.assertIn("finite", stderr.getvalue())
            executable.assert_not_called()
            wrapper.assert_not_called()


if __name__ == "__main__":
    unittest.main()
