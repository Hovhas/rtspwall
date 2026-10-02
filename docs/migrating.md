# Migrating from displaycameras, rpisurv, camplayer or OpenSurv

You used one of these tools and want a camera wall on a current Raspberry Pi OS. This page maps what you know to rtspwall.

> The names of the old settings below come from our research notes on each project. Check them against your own config. If something does not match, tell us in [Discussions](https://github.com/Hovhas/rtspwall/discussions).

## Why the old tool stopped working

displaycameras, rpisurv and camplayer play video with `omxplayer` (or VLC with a MMAL output). That player was deprecated years ago, is 32-bit, and does not exist on Raspberry Pi OS Bookworm or Trixie. Their advice is to install the old Buster image, which no longer gets security updates.

rtspwall does the same job with the Pi 4's H.264 decoder and the display hardware, through the current Linux graphics stack.

## Before you start

- You need a Pi 4 (not a Pi 3 or older, not a Pi 5).
- Your cameras must send H.264. Use sub-streams.
- Flash Raspberry Pi OS **Lite** 64-bit. Do not try to keep an old install.

Then follow [Getting started](getting-started.md). The rest of this page is for translating your config.

## Terms

| Old idea | In rtspwall |
|---|---|
| A window or screen position | A **cell** of `GRID`, or a manual tile `width\|height\|x\|y` |
| A list of camera feeds | One line per camera: `name\|url\|cell` |
| Rotation between feeds | Give cameras the **same cell**. They take turns every `ROTATE_SECONDS` |
| Camera password in the config | Same, in the URL. Keep it in `/etc/rtspwall/cameras.conf` (`0640`) |

## displaycameras

| displaycameras | rtspwall |
|---|---|
| `windows` and `window_positions` | `GRID=COLSxROWS`, or manual tiles in pixels |
| `camera_feeds` | Camera lines `name\|url\|cell` |
| `rotate` and `rotatedelay` | Cameras sharing a cell, and `ROTATE_SECONDS` |
| Off-screen windows for rotation | Not needed. All cameras of a group decode, and the switch is one screen update |

## rpisurv and OpenSurv

| rpisurv | rtspwall |
|---|---|
| `nr_of_columns` | `GRID=COLSxROWS` |
| `duration` | `ROTATE_SECONDS` |
| `force_coordinates` | Manual layout: `name\|url\|width\|height\|x\|y` |
| `gpu_mem=512` | `gpu_mem=256` is enough for 4 or more concurrent 1080p decoders. `rtspwall doctor --fix` sets it if your cameras need it |

OpenSurv is the successor of rpisurv. It is a different design: it runs a desktop and decodes with mpv. It is a good choice on an x86 mini PC.

## camplayer

camplayer's `[DEVICEx]` and `[SCREENx]` sections map to lines in `cameras.conf`. Each device becomes a camera line. Each screen layout becomes a `GRID`, or a set of manual tiles. camplayer's layout presets such as 1+5 are not built in: use manual tiles ([Configuration](configuration.md#manual-layout)).

## Example

An old config with four feeds in a 2x2 layout and one rotating tile becomes:

```
GRID=2x2
ROTATE_SECONDS=15

front-door|rtsp://viewer:PASSWORD@192.168.1.10:554/h264Preview_01_sub|1
driveway  |rtsp://viewer:PASSWORD@192.168.1.11:554/h264Preview_01_sub|2
garage    |rtsp://viewer:PASSWORD@192.168.1.12:554/h264Preview_01_sub|3
backyard  |rtsp://viewer:PASSWORD@192.168.1.13:554/h264Preview_01_sub|4
side-gate |rtsp://viewer:PASSWORD@192.168.1.14:554/h264Preview_01_sub|4
```

Instead of typing it, use `sudo rtspwall add NAME [CELL]` for each camera. It asks for the URL at a hidden prompt.

## What is better

- It runs on current Raspberry Pi OS, 64-bit.
- Rotation has no black frame. A camera that drops does not move the other cells.
- `probe` tells you if a stream fits before you add it. `doctor` finds the usual setup problems.
- A bad config does not restart in a loop, and a saved valid config restarts the wall by itself.

## What is missing

- **H.265.** Not supported yet ([FAQ](faq.md#my-cameras-only-send-h265)).
- **Keyboard or remote control.** The wall is display-only.
- **Two screens.** One screen at a time.
- **On-screen names and status.** A tile with a problem is black. The log says why.
- **Pi 3 and Pi 5.** Pi 4 only.
