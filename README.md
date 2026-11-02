# Terminal Lyrics

A Linux terminal app that shows lyrics for the song you are playing.
Built with C++17 and Shell scripts, with lyrics from LRCLIB.

## Install

On Debian or Ubuntu:

```bash
sudo apt install build-essential curl playerctl
```

You need internet access to load lyrics.

## Run

Start playing music in Spotify, VLC, or a browser that supports Linux media
controls. From the project folder, run:

```bash
./run.sh
```

The script builds the app automatically.

To choose a player, list available names, then use one with `--player`
(example: Brave):

```bash
playerctl --list-all
./run.sh --player brave
```

Use `./run.sh --help` to see all options.

## Controls

| Key | Action |
| --- | --- |
| Space | Play or pause |
| `n` / `b` | Next / previous song |
| Tab | Switch between Overview and Lyrics |
| Up / Down | Scroll lyrics |
| `f` | Follow playback again |
| `[` / `]` | Show lyrics later / earlier by 0.25 seconds |
| `0` | Reset lyric timing |
| `q` | Quit |

Synced lyrics scroll and highlight automatically. Plain lyrics need manual
scrolling. Some songs may have no lyrics available.

## Build and test

Build without starting the app:

```bash
./build.sh
```

Run the tests (no internet or music player needed):

```bash
./test.sh
```
