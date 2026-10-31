import curses
import json
import unicodedata
import unittest
from unittest.mock import patch

from terminal_lyrics.lyrics_service import fetch_lyrics
from terminal_lyrics.models import Track
from terminal_lyrics.ui import draw


def cell_width(text: str) -> int:
    return sum(
        0 if unicodedata.combining(character) else
        2 if unicodedata.east_asian_width(character) in ("W", "F") else 1
        for character in text
    )


class FakeWindow:
    def __init__(self, height: int = 32, width: int = 80, resizing: bool = False) -> None:
        self.height = height
        self.width = width
        self.resizing = resizing
        self.writes: list[tuple[int, int, str, int]] = []
        self.refreshed = False

    def erase(self) -> None:
        self.writes.clear()

    def getmaxyx(self) -> tuple[int, int]:
        return self.height, self.width

    def bkgd(self, *args: object) -> None:
        pass

    def addnstr(self, row: int, column: int, text: str, count: int, attr: int = 0) -> None:
        if self.resizing:
            raise curses.error("window resized while drawing")
        displayed = text[:count]
        cells = cell_width(displayed)
        if not (0 <= row < self.height and 0 <= column < self.width):
            raise AssertionError(f"Write starts outside the window: {row}, {column}")
        if column + cells > self.width:
            raise AssertionError(f"Write exceeds terminal columns: {displayed!r}")
        if row == self.height - 1 and column + cells == self.width and cells:
            raise AssertionError("Write touches the bottom-right terminal cell")
        self.writes.append((row, column, displayed, attr))

    def addstr(self, row: int, column: int, text: str, attr: int = 0) -> None:
        self.addnstr(row, column, text, len(text), attr)

    def hline(self, row: int, column: int, character: object, count: int, attr: int = 0) -> None:
        self.addnstr(row, column, "-" * count, count, attr)

    def refresh(self) -> None:
        if self.resizing:
            raise curses.error("window resized while refreshing")
        self.refreshed = True

    @property
    def text(self) -> str:
        return "\n".join(text for _, _, text, _ in self.writes)


class DrawTests(unittest.TestCase):
    def setUp(self) -> None:
        self.track = Track("A Beautiful Song", "A Singer", "The Album", "brave")
        self.colors = patch("terminal_lyrics.ui.curses.color_pair", return_value=0)
        self.colors.start()
        self.addCleanup(self.colors.stop)

    def test_wraps_lyric_without_losing_words(self) -> None:
        window = FakeWindow(height=34, width=42)
        words = [f"lyric-word-{index}" for index in range(8)]

        draw(window, self.track, [], [" ".join(words)], 0.0, "Playing")

        for word in words:
            self.assertIn(word, window.text)
        lyric_rows = {row for row, _, text, _ in window.writes if "lyric-word-" in text}
        self.assertGreater(len(lyric_rows), 1)

    def test_preserves_blank_lines_between_stanzas(self) -> None:
        window = FakeWindow()

        draw(window, self.track, [], ["STANZA-ONE", "", "STANZA-TWO"], 0.0, "Paused")

        first = next(row for row, _, text, _ in window.writes if "STANZA-ONE" in text)
        second = next(row for row, _, text, _ in window.writes if "STANZA-TWO" in text)
        self.assertGreaterEqual(second - first, 2)

    def test_highlights_current_lyric_without_terminal_colors(self) -> None:
        window = FakeWindow()
        lyrics = [(0.0, "EARLIER-LINE"), (5.0, "CURRENT-LINE"), (10.0, "LATER-LINE")]

        # Some monochrome terminals still return a numeric color-pair mask.
        # Only initialized theme pairs establish whether color is available.
        with (
            patch("terminal_lyrics.ui.curses.color_pair", return_value=1792),
            patch("terminal_lyrics.ui._theme_pairs", set()),
        ):
            draw(window, self.track, lyrics, [], 6.0, "Playing")

        current = [attr for _, _, text, attr in window.writes if "CURRENT-LINE" in text]
        self.assertTrue(current)
        self.assertTrue(any(attr & curses.A_REVERSE for attr in current))
        for _, _, text, attr in window.writes:
            if "EARLIER-LINE" in text or "LATER-LINE" in text:
                self.assertFalse(attr & curses.A_REVERSE)

    def test_plain_lyrics_can_scroll_to_last_line(self) -> None:
        window = FakeWindow(height=20)
        plain = [f"verse-line-{index:02}" for index in range(50)]

        start, maximum = draw(window, self.track, [], plain, 0.0, "Playing")

        self.assertEqual(start, 0)
        self.assertGreater(maximum, 0)
        self.assertNotIn(plain[-1], window.text)
        start, new_maximum = draw(
            window, self.track, [], plain, 0.0, "Playing", scroll=maximum
        )
        self.assertEqual((start, new_maximum), (maximum, maximum))
        self.assertIn(plain[-1], window.text)
        self.assertNotIn(plain[0], window.text)

    def test_overview_shows_track_metadata_instead_of_lyrics(self) -> None:
        window = FakeWindow()

        draw(
            window, self.track, [], ["LYRIC-CONTENT"], 44.0, "Playing", view="overview"
        )

        self.assertIn(self.track.title, window.text)
        self.assertIn(self.track.artist, window.text)
        self.assertIn(self.track.album, window.text)
        self.assertIn(self.track.player, window.text)
        self.assertNotIn("LYRIC-CONTENT", window.text)

    def test_unicode_and_small_windows_remain_inside_terminal_bounds(self) -> None:
        track = Track("世界の歌 " * 8, "歌手 e\u0301", "作品", "brave")
        plain = ["響く歌声 " * 20, "Another long lyric " * 10]
        for height, width in [(1, 1), (7, 29), (8, 30), (12, 32), (24, 80)]:
            with self.subTest(height=height, width=width):
                window = FakeWindow(height, width)

                draw(window, track, [], plain, 0.0, "Playing")

                self.assertTrue(window.refreshed)

    def test_resize_errors_during_draw_do_not_escape(self) -> None:
        window = FakeWindow(resizing=True)

        draw(window, self.track, [], ["A lyric"], 0.0, "Playing")


class PlainLyricsSpacingTests(unittest.TestCase):
    def test_fetch_preserves_internal_blank_lines_for_stanza_spacing(self) -> None:
        record = {
            "id": 1,
            "trackName": "Song",
            "artistName": "Artist",
            "plainLyrics": "\nFirst verse\n\nSecond verse\n",
        }
        with patch("terminal_lyrics.lyrics_service.urlopen") as request:
            response = request.return_value.__enter__.return_value
            response.read.return_value = json.dumps([record]).encode("utf-8")

            result = fetch_lyrics(Track("Song", "Artist"))

        self.assertEqual(result, ([], ["First verse", "", "Second verse"], ""))


if __name__ == "__main__":
    unittest.main()
