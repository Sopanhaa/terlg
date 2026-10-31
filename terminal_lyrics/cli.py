import argparse
import curses
import math
import shutil
import sys

from .ui import live


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Display lyrics for the currently playing Linux music."
    )
    parser.add_argument("--player", help="Use a specific playerctl/MPRIS player, e.g. spotify or vlc")
    parser.add_argument(
        "--refresh", type=float, default=0.5,
        help="Playback polling interval in seconds (default: 0.5)",
    )
    parser.add_argument(
        "--offset", type=float, default=0.0,
        help="Lyric timing adjustment in seconds; positive shows lyrics earlier (default: 0)",
    )
    args = parser.parse_args()
    if not math.isfinite(args.refresh) or args.refresh < 0.1:
        parser.error("--refresh must be at least 0.1 seconds")
    if not math.isfinite(args.offset):
        parser.error("--offset must be a finite number of seconds")
    if not shutil.which("playerctl"):
        print("playerctl is missing. Install it first; see README.md.", file=sys.stderr)
        return 1
    try:
        curses.wrapper(live, args.player, args.refresh, args.offset)
    except KeyboardInterrupt:
        pass
    return 0
