import math
import subprocess

from .models import Track

METADATA_SEPARATOR = "\x1f"
METADATA_FORMAT = METADATA_SEPARATOR.join(
    "{{" + field + "}}"
    for field in (
        "title", "artist", "album", "playerName", "status", "position",
        "mpris:length", "mpris:trackid",
    )
)


def run_playerctl(*args: str, player: str | None = None) -> str:
    command = ["playerctl"]
    if player:
        command += ["--player", player]
    command += list(args)
    result = subprocess.run(command, text=True, capture_output=True, timeout=2, check=False)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "No active music player found.")
    # Keep empty fields at either end of a formatted metadata snapshot.
    return result.stdout.rstrip("\r\n")


def _seconds(value: str) -> float | None:
    try:
        microseconds = float(value)
    except ValueError:
        return None
    if not math.isfinite(microseconds) or microseconds < 0:
        return None
    return microseconds / 1_000_000


def current_track(player: str | None) -> tuple[Track, float | None, str]:
    metadata = run_playerctl("metadata", "--format", METADATA_FORMAT, player=player)
    fields = metadata.split(METADATA_SEPARATOR)
    if not fields[0].strip():
        raise RuntimeError("No song is currently playing.")
    if len(fields) != 8:
        raise RuntimeError("Could not read the player's song metadata.")
    title, artist, album, player_name, status, position, duration, track_id = (
        field.strip() for field in fields
    )
    track = Track(
        title, artist, album, player_name or player or "",
        _seconds(duration) or 0.0, track_id,
    )
    return track, _seconds(position), status
