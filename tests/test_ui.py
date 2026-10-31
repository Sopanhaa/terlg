import unittest

from terminal_lyrics.ui import playback_command


class PlaybackCommandTests(unittest.TestCase):
    def test_space_toggles_pause_and_resume(self) -> None:
        self.assertEqual(playback_command(ord(" ")), "play-pause")

    def test_next_shortcut(self) -> None:
        self.assertEqual(playback_command(ord("n")), "next")
        self.assertEqual(playback_command(ord("N")), "next")

    def test_previous_shortcut(self) -> None:
        self.assertEqual(playback_command(ord("b")), "previous")
        self.assertEqual(playback_command(ord("B")), "previous")

    def test_other_keys_do_not_issue_player_commands(self) -> None:
        self.assertIsNone(playback_command(ord("x")))
        self.assertIsNone(playback_command(ord("p")))
        self.assertIsNone(playback_command(ord("q")))


if __name__ == "__main__":
    unittest.main()
