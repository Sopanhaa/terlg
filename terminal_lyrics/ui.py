import curses
import math
import subprocess
import time
import unicodedata
from functools import lru_cache

from .lyrics_loader import LyricsLoader
from .lyrics_service import active_line
from .models import Track
from .playback_clock import PlaybackClock
from .player import current_track, run_playerctl

TEXT, MUTED, ARTIST, ARTIST_END, PILL, SELECTED, CURRENT, RULE = range(1, 9)
_theme_pairs: set[int] = set()


def setup_theme(stdscr: curses.window) -> None:
    _theme_pairs.clear()
    if not curses.has_colors():
        return
    curses.start_color()
    if curses.COLORS >= 256:
        background = 235
        palette = [
            (255, background), (245, background), (140, background),
            (175, background), (252, 239), (255, background),
            (255, 242), (241, background),
        ]
    else:
        palette = [
            (curses.COLOR_WHITE, curses.COLOR_BLACK),
            (curses.COLOR_WHITE, curses.COLOR_BLACK),
            (curses.COLOR_MAGENTA, curses.COLOR_BLACK),
            (curses.COLOR_MAGENTA, curses.COLOR_BLACK),
            (curses.COLOR_WHITE, curses.COLOR_BLACK),
            (curses.COLOR_WHITE, curses.COLOR_BLACK),
            (curses.COLOR_BLACK, curses.COLOR_WHITE),
            (curses.COLOR_WHITE, curses.COLOR_BLACK),
        ]
    for pair, (foreground, background) in enumerate(palette, start=1):
        if pair >= curses.COLOR_PAIRS:
            break
        try:
            curses.init_pair(pair, foreground, background)
        except curses.error:
            continue
        _theme_pairs.add(pair)
    stdscr.bkgd(" ", _style(TEXT))


def _style(pair: int, extra: int = 0) -> int:
    try:
        color = curses.color_pair(pair) if pair in _theme_pairs else 0
    except curses.error:
        color = 0
    if not color and pair in (SELECTED, CURRENT):
        extra |= curses.A_REVERSE
    return color | extra


def _clean(text: str) -> str:
    return "".join(
        character if character.isprintable() else " "
        for character in text.expandtabs(4)
    )


def _width(text: str) -> int:
    return sum(
        0 if unicodedata.combining(character) else
        2 if unicodedata.east_asian_width(character) in ("W", "F") else 1
        for character in text
    )


def _clip(text: str, width: int, ellipsis: bool = False) -> str:
    text = _clean(text)
    if _width(text) <= width:
        return text
    available = max(0, width - (1 if ellipsis else 0))
    end = 0
    used = 0
    for index, character in enumerate(text):
        size = _width(character)
        if used + size > available:
            break
        used += size
        end = index + 1
    return text[:end] + ("…" if ellipsis and width > 0 else "")


@lru_cache(maxsize=2048)
def _wrap(text: str, width: int) -> tuple[str, ...]:
    remaining = _clean(text).strip()
    if not remaining or width < 1:
        return ("",)
    lines = []
    while _width(remaining) > width:
        prefix = _clip(remaining, width)
        split = prefix.rfind(" ")
        end = split if split > 0 else len(prefix)
        if not end:
            lines.append("?")
            remaining = remaining[1:]
            continue
        lines.append(remaining[:end])
        remaining = remaining[end:].lstrip()
    if remaining:
        lines.append(remaining)
    return tuple(lines)


def _put(stdscr: curses.window, row: int, column: int, text: str, attr: int = 0) -> None:
    height, width = stdscr.getmaxyx()
    available = width - column - (1 if row == height - 1 else 0)
    if not (0 <= row < height and 0 <= column < width) or available < 1:
        return
    text = _clip(text, available)
    try:
        stdscr.addnstr(row, column, text, len(text), attr)
    except curses.error:
        # A resize can happen between reading the dimensions and writing.
        pass


def _tabs(stdscr: curses.window, row: int, left: int, view: str, tall: bool) -> None:
    column = left
    for name in ("Overview", "Lyrics"):
        selected = name.casefold() == view
        attr = _style(SELECTED, curses.A_BOLD) if selected else _style(PILL)
        label = f" {name} "
        if tall:
            _put(stdscr, row, column, "╭" + "─" * len(label) + "╮", attr)
            _put(stdscr, row + 1, column, f"│{label}│", attr)
            _put(stdscr, row + 2, column, "╰" + "─" * len(label) + "╯", attr)
        else:
            _put(stdscr, row, column, f"({label})" if selected else f" {label} ", attr)
        column += len(label) + 4


def _refresh(stdscr: curses.window) -> None:
    try:
        stdscr.refresh()
    except curses.error:
        pass


def playback_command(key: int) -> str | None:
    if key == ord(" "):
        return "play-pause"
    if key in (ord("n"), ord("N")):
        return "next"
    if key in (ord("b"), ord("B")):
        return "previous"
    return None


def _time_label(position: float | None) -> str:
    if position is None:
        return "--:--"
    return f"{int(position // 60):02}:{int(position % 60):02}"


def draw(
    stdscr: curses.window,
    track: Track,
    lyrics: list[tuple[float, str]],
    plain: list[str],
    position: float | None,
    status: str,
    message: str = "",
    *,
    view: str = "lyrics",
    scroll: int | None = None,
    notice: str = "",
    lyric_offset: float = 0.0,
) -> tuple[int, int]:
    position = max(0.0, position) if position is not None and math.isfinite(position) else None
    stdscr.erase()
    height, width = stdscr.getmaxyx()
    if height < 8 or width < 30:
        _put(stdscr, 0, 0, "Please make the terminal larger (at least 30x8).")
        _refresh(stdscr)
        return 0, 0

    padding = 4 if width >= 70 else 2
    content_width = min(100, width - padding * 2)
    left = (width - content_width) // 2
    compact = height < 18
    top = 0 if compact else 1
    title_lines = _wrap(track.title, content_width)
    title_limit = 2 if height >= 24 else 1
    for index, line in enumerate(title_lines[:title_limit]):
        if index == title_limit - 1 and len(title_lines) > title_limit:
            line = _clip(line + " …", content_width, ellipsis=True)
        _put(stdscr, top + index, left, line, _style(TEXT, curses.A_BOLD))

    artist_row = top + min(len(title_lines), title_limit) + (1 if height >= 24 else 0)
    artist = _clip(f"Song by {track.artist or 'Unknown artist'}", content_width, ellipsis=True)
    split = artist.rfind(" ", 0, max(8, len(artist) // 2))
    split = max(0, split + 1)
    _put(stdscr, artist_row, left, artist[:split], _style(ARTIST))
    _put(stdscr, artist_row, left + _width(artist[:split]), artist[split:], _style(ARTIST_END))

    tab_row = artist_row + (1 if compact else 2)
    tall_tabs = height >= 26
    _tabs(stdscr, tab_row, left, view, tall_tabs)
    divider_row = tab_row + (3 if tall_tabs else 1) + (0 if compact else 1)
    _put(stdscr, divider_row, left, "─" * content_width, _style(RULE))
    body_row = divider_row + (2 if height >= 24 else 1)
    status_row = height - (3 if height >= 14 else 2)
    available = max(1, status_row - body_row - (0 if compact else 1))

    lyric_position = position + lyric_offset if position is not None else None
    focused = active_line(lyrics, lyric_position) if lyrics and lyric_position is not None else -1
    rows: list[tuple[int, str]] = []
    if view == "overview":
        overview = [
            "Now playing", "",
            f"Status    {status or 'Stopped'}",
            f"Position  {_time_label(position)}",
            f"Player    {track.player or 'MPRIS'}",
            f"Album     {track.album or 'Unknown album'}",
            f"Lyrics    {len(lyrics) if lyrics else len(plain)} "
            f"{'synchronized' if lyrics else 'plain text'} lines",
        ]
        if message:
            overview.extend(["", message])
        rows = [(-1, part) for line in overview for part in _wrap(line, content_width)]
    elif message or not (lyrics or plain):
        rows = [(-1, part) for part in _wrap(message or "No lyrics to display.", content_width)]
    else:
        lines = [line for _, line in lyrics] if lyrics else plain
        rows = [
            (index, part) for index, line in enumerate(lines)
            for part in _wrap(line, content_width)
        ]

    maximum = max(0, len(rows) - available)
    focus_row = next((index for index, (lyric, _) in enumerate(rows) if lyric == focused), 0)
    automatic = max(0, min(focus_row - available // 3, maximum)) if lyrics and view == "lyrics" else 0
    start = max(0, min(automatic if scroll is None else scroll, maximum))
    for offset, (lyric, line) in enumerate(rows[start:start + available]):
        highlighted = view == "lyrics" and bool(lyrics) and lyric == focused and focused >= 0
        attr = _style(CURRENT, curses.A_BOLD) if highlighted else _style(TEXT)
        if highlighted and line:
            _put(stdscr, body_row + offset, left - 1, f" {line} ", attr)
        else:
            _put(stdscr, body_row + offset, left, line, attr)

    playback = f"{status or 'Stopped'}  ·  {_time_label(position)}  ·  {track.player or 'MPRIS'}"
    _put(stdscr, status_row, left, _clip(notice or playback, content_width, ellipsis=True), _style(MUTED))
    mode = "Synced lyrics" if lyrics else "Plain lyrics" if plain else "Waiting for lyrics"
    if lyrics and position is None:
        mode = "Playback time unavailable"
    elif lyrics and lyric_offset:
        mode = f"Lyrics {lyric_offset:+.2f}s"
    if scroll is not None:
        mode = f"{start + 1}–{min(start + available, len(rows))} / {len(rows)}"
    if not notice and _width(playback) + len(mode) + 4 <= content_width:
        _put(stdscr, status_row, left + content_width - len(mode), mode, _style(MUTED))
    footer = "Space pause  n/b tracks  ↑↓ scroll  Tab view  f follow  [/] sync  q quit"
    if content_width < len(footer):
        footer = "Space pause  ↑↓ scroll  Tab view  q quit"
    if content_width < len(footer):
        footer = "Space pause  Tab  q quit"
    _put(stdscr, height - 1, left, _clip(footer, content_width), _style(MUTED))
    _refresh(stdscr)
    return start, maximum


def live(
    stdscr: curses.window, player: str | None, refresh: float, lyric_offset: float = 0.0,
) -> None:
    try:
        curses.curs_set(0)
    except curses.error:
        pass
    stdscr.nodelay(True)
    stdscr.keypad(True)
    setup_theme(stdscr)

    last_key: tuple[str, str, str, int] | None = None
    lyrics: list[tuple[float, str]] = []
    plain: list[str] = []
    message = "Waiting for a song..."
    track = Track("Waiting for music", "Play a song in Spotify, VLC, or another MPRIS player")
    position: float | None = None
    status = ""
    view = "lyrics"
    scroll: int | None = None
    viewport_start = viewport_maximum = 0
    next_poll = 0.0
    last_tick: tuple[int | None, int] | None = None
    playback_error = ""
    clock = PlaybackClock()
    loader = LyricsLoader()

    try:
        while True:
            key = stdscr.getch()
            if key in (ord("q"), ord("Q")):
                return
            redraw = key != -1
            if key in (ord("\t"), curses.KEY_BTAB, curses.KEY_LEFT, curses.KEY_RIGHT):
                view = "overview" if view == "lyrics" else "lyrics"
                scroll = None
            elif key in (ord("1"), ord("2")):
                view = "overview" if key == ord("1") else "lyrics"
                scroll = None
            elif key in (curses.KEY_UP, curses.KEY_PPAGE):
                scroll = max(0, viewport_start - (1 if key == curses.KEY_UP else 5))
            elif key in (curses.KEY_DOWN, curses.KEY_NPAGE):
                scroll = min(viewport_maximum, viewport_start + (1 if key == curses.KEY_DOWN else 5))
            elif key == curses.KEY_HOME:
                scroll = 0
            elif key == curses.KEY_END:
                scroll = viewport_maximum
            elif key in (ord("f"), ord("F")):
                scroll = None
            elif key in (ord("]"), ord("+"), ord("=")):
                lyric_offset = round(lyric_offset + 0.25, 2)
            elif key in (ord("["), ord("-"), ord("_")):
                lyric_offset = round(lyric_offset - 0.25, 2)
            elif key == ord("0"):
                lyric_offset = 0.0
            control_notice = ""
            command = playback_command(key)
            if command:
                try:
                    run_playerctl(command, player=player)
                except (RuntimeError, FileNotFoundError, subprocess.TimeoutExpired) as exc:
                    control_notice = f"Playback control failed: {exc}"
            if command or time.monotonic() >= next_poll:
                try:
                    poll_started = time.monotonic()
                    latest, reported_position, status = current_track(player)
                    sampled_at = (poll_started + time.monotonic()) / 2
                    if (latest.player, latest.track_id) != (track.player, track.track_id):
                        scroll = None
                    clock.update(latest, reported_position, status, now=sampled_at)
                    playback_error = ""
                    track = latest
                    if latest.lyrics_key != last_key:
                        lyrics, plain = [], []
                        message = "Loading lyrics..."
                        loader.request(track)
                        last_key = latest.lyrics_key
                        scroll = None
                except (RuntimeError, FileNotFoundError, subprocess.TimeoutExpired) as exc:
                    playback_error = str(exc)
                next_poll = time.monotonic() + refresh
                redraw = True
            result = loader.poll(track)
            if result is not None:
                lyrics, plain, message = result
                redraw = True
            position = clock.position()
            lyric_position = position + lyric_offset if position is not None else None
            tick = (
                int(position) if position is not None else None,
                active_line(lyrics, lyric_position) if lyrics and lyric_position is not None else -1,
            )
            if tick != last_tick:
                redraw = True
                last_tick = tick
            if redraw:
                viewport_start, viewport_maximum = draw(
                    stdscr, track, lyrics, plain, position, status, playback_error or message,
                    view=view, scroll=scroll, notice=control_notice, lyric_offset=lyric_offset,
                )
            # Keyboard handling stays responsive even with a long --refresh.
            time.sleep(min(0.05, max(0.0, next_poll - time.monotonic())))
    finally:
        loader.close()
