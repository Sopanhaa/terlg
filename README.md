# Terminal Lyrics

A Linux terminal app that displays lyrics for the song currently playing in an
MPRIS-compatible music player or browser. It reads playback information through
`playerctl` and searches LRCLIB for synchronized or plain lyrics.

## Features

- Detects song, artist, album, playback status, and position from the player.
- Shows synchronized lyrics with automatic scrolling and a gray current-line
  highlight, or plain lyrics when timing is unavailable.
- Advances the lyric clock between browser/player polls and corrects it after
  pauses, seeks, or track changes; supports manual timing adjustments for videos.
- Provides a dark layout with a bold song heading, purple/pink artist text, and
  **Overview** and **Lyrics** tabs.
- Wraps long lines, preserves verse spacing, and supports manual scrolling.
- Loads lyrics in the background so the screen and keyboard remain responsive;
  successful lookups are cached for up to 64 songs while the app is running.
- Handles common video-title formats, cover descriptions, and channel-name
  spacing when searching for lyrics.
- Controls play/pause, next track, and previous track from the keyboard.

## Platform and player support

| Platform | Current support |
| --- | --- |
| Linux | Supported with `playerctl` and a compatible MPRIS player. |
| Native Windows | Not supported. A Windows playback backend and terminal compatibility work are required. |

Spotify, VLC, and browser playback such as Brave are examples of possible music
sources. Compatibility depends on the application exposing its media session
through MPRIS; available metadata and playback controls can vary.

List the players available on your system:

```bash
playerctl --list-all
```

Use a name from that list with `--player` to choose a specific source.

## Requirements

- Python 3.10 or later
- Python's standard-library `curses` module, normally included with Linux Python
  installations
- The Linux package `playerctl`
- A running MPRIS-compatible music player or browser with an active media session
- Internet access to look up lyrics through LRCLIB
- A terminal at least 30 columns wide and 8 rows high

The full theme uses 256 colors. Simpler color and monochrome terminals use
fallback styles. Font size is controlled by your terminal.

On Debian or Ubuntu, install the required packages:

```bash
sudo apt install python3 playerctl
```

Check that `python3 --version` reports Python 3.10 or newer. No `pip` dependencies
are required; the project uses Python's standard library.

## Run

Start playing music, then run this from the project directory:

```bash
python3 lyrics.py
```

Or run it as a module:

```bash
python3 -m terminal_lyrics
```

For example, select Brave and refresh the display once per second:

```bash
python3 lyrics.py --player brave --refresh 1
```

| Option | Behavior |
| --- | --- |
| `--player NAME` | Select a specific player; otherwise use `playerctl`'s default selection. |
| `--refresh SECONDS` | Set the playback polling interval; default `0.5`, minimum `0.1`. Lyric highlighting advances between polls; browser seeks and pauses are detected at the next poll. |
| `--offset SECONDS` | Adjust lyric timing; positive values show lyrics earlier, negative values later. Default `0`. |
| `--help` | Show command-line help. |

## Keyboard controls

| Key | Action |
| --- | --- |
| Space | Play/pause. |
| `n` / `b` | Next / previous track. |
| Tab, Shift+Tab, Left, Right | Switch between Overview and Lyrics. |
| `1` / `2` | Select Overview / Lyrics directly. |
| Up / Down | Scroll one displayed row. |
| Page Up / Page Down | Scroll five displayed rows. |
| Home / End | Jump to the beginning / end. |
| `f` | Resume following playback for synchronized lyrics; return to the top for plain lyrics. |
| `]`, `+`, `=` | Advance lyrics by 0.25 seconds when the highlight is late. |
| `[`, `-`, `_` | Delay lyrics by 0.25 seconds when the highlight is early. |
| `0` | Reset the lyric timing adjustment to zero. |
| `q` | Quit. |

The Overview tab shows playback status, position, player, album, and lyric
availability. The Lyrics tab shows the song's text and follows the active line
when synchronized lyrics are available. Manual scrolling pauses automatic
following until you press `f` or a new track loads.

## Browser playback timing

The app reads the browser's status and position together, then advances its
display clock between reads while playback is active. Pausing freezes that
clock; the next browser sample corrects its position after a seek or resume.
If a position sample is temporarily missing, the app keeps its last known
timeline. If no position has been available for the current track, the time
shows `--:--` and no lyric is highlighted.

Synchronized lyrics can still differ from a video's timing when the video adds
an intro. Use `]` to move the highlight earlier or `[` to move it later. The
current adjustment appears beside the playback status when there is room.
These keys adjust the lyrics without seeking the browser. Timing adjustments
last for the session; `0` clears them.

For example, start with lyrics 1.5 seconds earlier:

```bash
python3 lyrics.py --player brave --offset 1.5
```

Only timestamped lyrics can follow playback. The **Plain lyrics** mode has no
timing information and must be scrolled manually.

## Lyrics lookup and limitations

Video titles such as `Habits - Vintage 1930's Jazz Tove Lo Cover ft. Haley
Reinhart` are searched using the song title (`Habits`) as well as the full video
title. Recognizable video titles are searched using the extracted song and
artist first, followed by channel-name spacing variants and title-only fallbacks.
Ordinary song metadata is searched directly before trying inferred titles.

The screen shows **Loading lyrics...** during a lookup. Changing tracks cancels
obsolete searches, and results from an earlier track cannot replace the current
song's lyrics. Revisiting a successfully loaded song uses the in-memory cache;
the cache is cleared when the app exits.

The app prefers matching performers and titles, using the browser's track
duration to help choose between recordings. Recordings with different lengths
are cached separately. Duration is a preference rather than an exact recording
check: intros, outros, and different performances can still affect timing.
When only another artist's
version or a title variation is available, lyrics appear without timing because
the recording can have a different arrangement. Matching metadata does not
guarantee that timestamps fit every performance or cover.

Lyrics availability and first-load speed depend on LRCLIB. The app uses shorter
request timeouts and a search budget to limit retries, and displays messages for
missing lyrics, connection problems, rate limits, and service errors. Lyrics are
loaded when track metadata changes; changing `--refresh` does not trigger
another search for the same track. Quitting does not wait for a lyrics request.

## Troubleshooting

| Problem | What to check |
| --- | --- |
| `playerctl is missing` | Install `playerctl` using your distribution's package manager. |
| No active player or song | Start playback and check `playerctl --list-all`; use `--player NAME` if needed. |
| Wrong player is selected | Pass the intended player name with `--player`. |
| Lyrics were not found | The track may be missing from LRCLIB, or its title/artist metadata may not match. |
| HTTP 503, rate limit, or connection error | Wait for the service or connection to recover, then restart the app to retry the same song. |
| Lyrics stop following playback | Press `f` to leave manual scrolling mode. |
| Synced lyrics are consistently late or early | Use `]` to advance or `[` to delay the highlight; reset with `0`, or start with `--offset SECONDS`. |
| Playback time shows `--:--` | The selected browser/player has not supplied a usable position; check the active media session and `--player` selection. |
| Plain lyrics do not highlight | This result has no verified timestamps; use Up/Down to scroll. |
| Text does not fit | Enlarge the terminal; lyric lines wrap and the layout adapts to its size. |

## Project layout

- `terminal_lyrics/player.py`: `playerctl` integration and track metadata
- `terminal_lyrics/playback_clock.py`: playback interpolation, pause handling, and seek correction
- `terminal_lyrics/lyrics_service.py`: LRCLIB search and LRC parsing
- `terminal_lyrics/lyrics_loader.py`: background loading, cancellation, and session cache
- `terminal_lyrics/ui.py`: curses display and keyboard controls
- `terminal_lyrics/cli.py`: command-line arguments and startup checks
- `terminal_lyrics/models.py`: track metadata and change detection
- `lyrics.py` and `terminal_lyrics/__main__.py`: application entry points
- `tests/`: standard-library unit tests

## Tests

Run the test suite from the project directory:

```bash
python3 -m unittest discover -s tests
```

Tests cover lyric parsing, video and cover searches, fallback matching, search
budgets, cancellation, background loading, caching, playback shortcuts, wrapping,
verse spacing, scrolling, playback clocks, browser position units, duration
matching, timing controls, monochrome highlighting, Unicode bounds, and terminal
resize handling. Service and player requests are mocked in the tests.

