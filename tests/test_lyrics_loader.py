import threading
import time
import unittest
from unittest.mock import patch

from terminal_lyrics.lyrics_loader import LyricsLoader
from terminal_lyrics.models import Track


RESULT = ([(1.0, "Loaded lyric")], [], "")


class LyricsLoaderTests(unittest.TestCase):
    def wait_for_result(self, loader: LyricsLoader, track: Track):
        deadline = time.monotonic() + 1.0
        pause = threading.Event()
        while time.monotonic() < deadline:
            result = loader.poll(track)
            if result is not None:
                return result
            pause.wait(0.005)
        self.fail(f"Lookup did not finish for {track.title!r}")

    def test_request_and_poll_return_while_lookup_is_blocked(self) -> None:
        entered = threading.Event()
        release = threading.Event()
        track = Track("Song", "Artist")
        workers = []

        def fetch(track, *, cancel_event):
            workers.append(threading.current_thread())
            entered.set()
            release.wait(2.0)
            return RESULT

        with patch("terminal_lyrics.lyrics_loader.fetch_lyrics", side_effect=fetch):
            loader = LyricsLoader()
            try:
                loader.request(track)
                self.assertTrue(entered.wait(1.0))
                self.assertTrue(workers[0].daemon, "A lookup must not keep the process alive after quitting")
                self.assertIsNone(loader.poll(track))
                release.set()
                self.assertEqual(self.wait_for_result(loader, track), RESULT)
            finally:
                release.set()
                loader.close()

    def test_superseded_result_is_ignored_and_only_latest_pending_track_runs(self) -> None:
        first = Track("First", "Artist")
        skipped = Track("Skipped", "Artist")
        latest = Track("Latest", "Artist")
        entered = threading.Event()
        release = threading.Event()
        calls = []
        cancellations = []

        def fetch(track, *, cancel_event):
            calls.append(track)
            if track == first:
                cancellations.append(cancel_event)
                entered.set()
                release.wait(2.0)
            return [], [track.title], ""

        with patch("terminal_lyrics.lyrics_loader.fetch_lyrics", side_effect=fetch):
            loader = LyricsLoader()
            try:
                loader.request(first)
                self.assertTrue(entered.wait(1.0))
                loader.request(skipped)
                loader.request(latest)
                self.assertTrue(cancellations[0].is_set())
                release.set()

                self.assertEqual(self.wait_for_result(loader, latest), ([], ["Latest"], ""))
                self.assertIsNone(loader.poll(first))
                self.assertEqual(calls, [first, latest])
            finally:
                release.set()
                loader.close()

    def test_successful_lookup_is_reused_by_track_key(self) -> None:
        track = Track("Song", "Artist", player="brave")
        same_song = Track("SONG", "ARTIST", player="vlc")

        with patch("terminal_lyrics.lyrics_loader.fetch_lyrics", return_value=RESULT) as fetch:
            loader = LyricsLoader()
            try:
                loader.request(track)
                self.assertEqual(self.wait_for_result(loader, track), RESULT)
                loader.request(same_song)
                self.assertEqual(self.wait_for_result(loader, same_song), RESULT)
                self.assertEqual(fetch.call_count, 1)
            finally:
                loader.close()

    def test_transient_error_is_not_cached(self) -> None:
        track = Track("Song", "Artist")
        failure = ([], [], "Lyrics service returned HTTP 503.")

        with patch(
            "terminal_lyrics.lyrics_loader.fetch_lyrics", side_effect=[failure, RESULT]
        ) as fetch:
            loader = LyricsLoader()
            try:
                loader.request(track)
                self.assertEqual(self.wait_for_result(loader, track), failure)
                loader.request(track)
                self.assertEqual(self.wait_for_result(loader, track), RESULT)
                self.assertEqual(fetch.call_count, 2)
            finally:
                loader.close()

    def test_different_recording_duration_is_not_served_from_previous_cache(self) -> None:
        original = Track("Song", "Artist", duration=180.0)
        extended = Track("Song", "Artist", duration=240.0)
        with patch("terminal_lyrics.lyrics_loader.fetch_lyrics", return_value=RESULT) as fetch:
            loader = LyricsLoader()
            try:
                for track in (original, extended, original):
                    loader.request(track)
                    self.assertEqual(self.wait_for_result(loader, track), RESULT)
                self.assertEqual([call.args[0] for call in fetch.call_args_list], [original, extended])
            finally:
                loader.close()

    def test_cache_evicts_least_recently_used_song(self) -> None:
        first, second, third = [Track(title, "Artist") for title in ("First", "Second", "Third")]

        with patch("terminal_lyrics.lyrics_loader.fetch_lyrics", return_value=RESULT) as fetch:
            loader = LyricsLoader(cache_size=2)
            try:
                for track in (first, second, first, third, second):
                    loader.request(track)
                    self.assertEqual(self.wait_for_result(loader, track), RESULT)
                self.assertEqual([call.args[0] for call in fetch.call_args_list], [first, second, third, second])
            finally:
                loader.close()

    def test_close_does_not_wait_for_a_blocked_lookup(self) -> None:
        entered = threading.Event()
        release = threading.Event()
        closed = threading.Event()
        cancellation = []

        def fetch(track, *, cancel_event):
            cancellation.append(cancel_event)
            entered.set()
            release.wait(2.0)
            return RESULT

        with patch("terminal_lyrics.lyrics_loader.fetch_lyrics", side_effect=fetch):
            loader = LyricsLoader()
            try:
                loader.request(Track("Song", "Artist"))
                self.assertTrue(entered.wait(1.0))

                def close():
                    loader.close()
                    closed.set()

                closer = threading.Thread(target=close, daemon=True)
                closer.start()
                self.assertTrue(closed.wait(1.0), "Closing waited for the network worker")
                self.assertTrue(cancellation[0].is_set())
            finally:
                release.set()
                loader.close()


if __name__ == "__main__":
    unittest.main()
