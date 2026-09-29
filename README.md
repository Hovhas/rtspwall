# rpi4-rtsp

An RTSP camera wall for the Raspberry Pi 4. Every camera is hardware-decoded and shown on its own display plane, with no X11, no Wayland and no frame copies.

[![CI](https://github.com/Hovhas/rpi4-rtsp/actions/workflows/ci.yml/badge.svg)](https://github.com/Hovhas/rpi4-rtsp/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Platform: Raspberry Pi 4](https://img.shields.io/badge/platform-Raspberry%20Pi%204-c51a4a.svg)

<!-- TODO: add demo GIF (docs/demo.gif) and reference it here with an image link once it exists. -->
*Demo GIF coming soon.*

Measured on a Pi 4 (8 GB) with six cameras on one 1080p60 HDMI display (four tiles, two of them rotating): **CPU 2-13 % (typically 6-9 %), 0.0-0.2 % dropped frames, about 150 ms latency** from the network to the screen.

## Why

- **The Pi 4 can do this in hardware.** The H.264 decoder and the display's hardware video scaler (HVS) do all the pixel work. The CPU only demuxes RTSP and schedules frames.
- **No display server.** rpi4-rtsp is the DRM/KMS client. You can run it on a Raspberry Pi OS Lite install that has no desktop.
- **Smooth, not just live.** Each camera has its own jitter buffer, and a PLL evens out recorders that timestamp frames in pairs. Details in [docs/architecture.md](docs/architecture.md).
- **Built to run unattended.** A watchdog reconnects a silent stream after 5 s. In a test that blocked all cameras for 15 s, the wall recovered 4-6 s after the block was lifted.

### How it compares

Short and, we hope, fair. Other projects solve different problems, and we only claim what we have measured or can read from the code.

| | rpi4-rtsp | [displaycameras](https://github.com/Anonymousdog/displaycameras) | mpv / VLC wall | Frigate Birdseye |
|---|---|---|---|---|
| What it is | Video wall for the Pi 4 | Video wall for the Pi 3 | General players, one per camera | View inside an NVR |
| Decode | Pi 4 V4L2 hardware decoder | omxplayer (VideoCore) | Depends on setup; often software on a Pi 4 | Not a display path for a Pi |
| Needs a display server | No (DRM/KMS) | No (DispmanX) | Usually X11 or Wayland | Browser or app |
| Works on current Raspberry Pi OS | Yes (Bookworm, Trixie) | No: omxplayer relies on DispmanX/MMAL, removed in Bullseye | Yes | Yes |
| Frame copies per stream | None | None | At least one in our X11 tests | n/a |
| Rotating cameras in one tile | Yes, no black frame | Not covered here | Scripted by hand | n/a |

Frigate is an NVR with a web UI, not a wall renderer, and the two work well together: point rpi4-rtsp at a go2rtc/Frigate restream so the NVR and the wall share one camera connection.

## Features

- Grid layout (`GRID=2x2` up to `8x8`, max 16 cameras) or manual pixel placement.
- Rotation groups: cameras on the same tile take turns every `ROTATE_SECONDS`. All of them keep decoding, so the switch happens in one atomic commit with no black frame. A camera with no fresh frame for more than 3 s is skipped.
- Per-camera jitter buffer with a sliding-minimum anchor and a PLL.
- Compositor driven by the display's vblank.
- RTSP watchdog: 5 s without data triggers a reconnect.
- `--check-config` validates a config and prints the resulting layout without touching the display, decoder or network.
- A statistics line per camera every 60 s in the journal.
- Credentials in URLs (passwords, query strings and token-like path segments such as UniFi Protect tokens) are masked in log output.

## Requirements

| Item | Requirement |
|---|---|
| Board | Raspberry Pi 4 Model B (tested). Pi 400 and CM4 should work but are **untested**. |
| Pi 5 | **Not supported.** It has no H.264 hardware decoder. |
| OS | Raspberry Pi OS Bookworm or Trixie, 64-bit **Lite**. Tested on Trixie. A desktop session that holds DRM master on the same output will block rpi4-rtsp. |
| Video | H.264 only. H.265/HEVC is not supported yet. |
| Libraries | FFmpeg 5.1-7.1 and libdrm >= 2.4.113 (what Bookworm and Trixie ship). Bullseye is not supported. |
| Firmware | `gpu_mem=256` in `config.txt` for 4 or more concurrent 1080p streams. The source `install.sh` sets it; the `.deb` and the tarball only warn (see [Quick start](#quick-start)). |
| Decoder budget | The Pi 4 H.264 decoder is specified for about 1080p60 in total. Examples: 4 x 1080p15 or 6 x 576p at 25-30 fps. Use the camera's sub-stream where you can. |

## Quick start

Download the package for your OS from [GitHub Releases](https://github.com/Hovhas/rpi4-rtsp/releases). The file is named `rpi4-rtsp_<version>_<bookworm|trixie>_arm64.deb`.

```bash
sudo apt install ./rpi4-rtsp_<version>_<bookworm|trixie>_arm64.deb
printf '[all]\ngpu_mem=256\n' | sudo tee -a /boot/firmware/config.txt && sudo reboot   # skip if already >= 256
sudo nano /etc/rpi4-rtsp/cameras.conf          # add your camera URLs
sudo rpi4-rtsp --check-config /etc/rpi4-rtsp/cameras.conf
sudo systemctl enable --now rpi4-rtsp
journalctl -u rpi4-rtsp -f
```

The package does not start the service and does not edit `config.txt`; it prints a warning if `gpu_mem` is too low. The service runs as the system user `rpi4-rtsp` (groups `video` and `render`).

Other ways to install:

- **Tarball** from the same Releases page (`rpi4-rtsp_<version>_<dist>_arm64.tar.gz`): extract it and run the `install.sh` inside.
- **From source:**
  ```bash
  git clone https://github.com/Hovhas/rpi4-rtsp && cd rpi4-rtsp
  sudo ./install.sh          # options: --no-gpu-mem, --no-build-deps
  ```
  `install.sh` installs build dependencies, builds, runs the tests, installs the binary and the unit, and sets `gpu_mem=256` (backing up `config.txt`; a reboot is needed). It does not enable or start the service. Remove a source install with `sudo ./uninstall.sh [--purge]`.

## Configuration

The config file is `/etc/rpi4-rtsp/cameras.conf` (mode 0640, `root:rpi4-rtsp`). A commented example is in [examples/cameras.conf](examples/cameras.conf). Lines starting with `#` are comments. Global settings are `KEY=VALUE` lines and can appear anywhere; an unknown key gives a warning, not an error.

| Key | Default | Meaning |
|---|---|---|
| `GRID` | not set | `COLSxROWS` (up to 8x8). Not set means manual layout. |
| `BUFFER_MS` | `160` | How far ahead of the stream's timestamps each frame is scheduled. Higher is smoother with more latency, lower is snappier with more skipped late frames. |
| `ROTATE_SECONDS` | `15` | Seconds each camera is shown within a rotation group. |
| `DECODER` | `/dev/video10` | V4L2 H.264 decoder device (bcm2835-codec). |
| `CONNECTOR` | first connected | DRM connector name, for example `HDMI-A-1`. |
| `DRM_DEVICE` | auto | DRM card, for example `/dev/dri/card1`. Auto picks the first `/dev/dri/cardN` with a connected display. |
| `FFMPEG_LOGLEVEL` | `error` | `quiet`, `error`, `warning` or `info`. Raise to `warning` to see messages such as RTP packet loss when debugging. |

### Grid layout

Camera lines are `name|url|cell[|delay_ms]`. Cells are numbered from 1, row by row. For `GRID=2x2`:

```
+-----------+-----------+
|     1     |     2     |
+-----------+-----------+
|     3     |     4     |
+-----------+-----------+
```

Tile edges are placed at `floor(i * width / cols)`, so tiles cover the whole screen with no gaps. If the size does not divide evenly, tiles differ by at most one pixel (1366 / 3 gives 455, 455, 456).

`delay_ms` is optional extra delay (-10000 to 10000 ms) for one camera on top of `BUFFER_MS`, for example when its network path is consistently slower.

### Rotation

Cameras given the **same cell** form a rotation group. They take turns for `ROTATE_SECONDS` each, and the first one listed starts visible. Every camera in a group still needs its own decoder instance and overlay plane, so rotation saves screen space, not decoder capacity.

```
GRID=2x2
ROTATE_SECONDS=15

front-door|rtsp://viewer:CHANGE_ME@192.168.1.10:554/stream1|1
driveway  |rtsp://viewer:CHANGE_ME@192.168.1.11:554/stream1|2
garage    |rtsp://viewer:CHANGE_ME@192.168.1.12:554/stream1|3
side-gate |rtsp://viewer:CHANGE_ME@192.168.1.14:554/stream1|3
```

(A `|` cannot appear inside a URL, because it separates the fields.)

### Manual layout

Leave out `GRID` and give each camera a tile in screen pixels: `name|url|width|height|x|y[|delay_ms]`. The tile is the size on screen; the display hardware scales the stream. Cameras with identical `width|height|x|y` form a rotation group. The two line formats cannot be mixed in one file.

```
front-door|rtsp://viewer:CHANGE_ME@192.168.1.10:554/stream1|960|540|0|0
driveway  |rtsp://viewer:CHANGE_ME@192.168.1.11:554/stream1|960|540|960|0
```

### Check a config

```bash
sudo rpi4-rtsp --check-config /etc/rpi4-rtsp/cameras.conf
sudo rpi4-rtsp --check-config --mode 1280x720 /etc/rpi4-rtsp/cameras.conf   # other screen size
```

`sudo` is needed only because the config is readable by root and the `rpi4-rtsp` user. It prints the interpreted layout (credentials masked), one tile per line, and a summary such as `OK: 6 cameras, 4 tiles, 2 rotation groups; needs 6 overlay planes and 6 concurrent decoder instances`. It exits non-zero on errors, with a `line N: ...` message.

Full command line:

```
Usage: rpi4-rtsp [CONFIG]
       rpi4-rtsp --check-config [--mode WxH] [CONFIG]
       rpi4-rtsp --help | --version
```

## Camera URL cheat sheet

Replace the placeholders. Sub-streams are strongly recommended (see [Requirements](#requirements)). Paths differ between models and firmware versions, so treat these as starting points and check your vendor's documentation.

| Source | URL pattern |
|---|---|
| UniFi Protect | `rtsp://<nvr-ip>:7447/<token>` - enable RTSP per camera channel in Protect; the token is in the URL Protect shows. |
| Reolink | `rtsp://<user>:<pass>@<ip>:554/h264Preview_01_sub` (`_main` for the main stream) |
| Hikvision | `rtsp://<user>:<pass>@<ip>:554/Streaming/Channels/102` (`101` is the main stream) |
| Dahua / Amcrest | `rtsp://<user>:<pass>@<ip>:554/cam/realmonitor?channel=1&subtype=1` (`subtype=0` is the main stream) |
| go2rtc / Frigate restream | `rtsp://<host>:8554/<stream-name>` |

The camera must deliver H.264. If a camera offers both codecs, pick H.264 for the stream you give to rpi4-rtsp.

## How it works

```
RTSP (libavformat, demux only)
  -> V4L2 M2M H.264 decoder (bcm2835-codec, /dev/video10)
  -> dmabuf export straight from the decoder's buffer
  -> drmModeAddFB2
  -> one DRM/KMS overlay plane per camera
  -> composed and scaled by the display hardware (HVS), one atomic commit per vblank
```

No frame is copied by the CPU or GPU and no GPU textures are involved: the decoder writes the buffer and the display reads it. For jitter buffering, the anchor and PLL, the vblank-driven compositor, rotation inside one atomic commit and teardown order, see [docs/architecture.md](docs/architecture.md).

## Monitoring

```bash
journalctl -u rpi4-rtsp -f
journalctl -u rpi4-rtsp | grep ': 60s'
```

Every 60 s each camera logs one line:

```
front-door: 60s dec=25.0 shown=25.0 dropped=0(0.0%) late=0 depth=4.1 jitter p50=3 p95=12ms latency=150ms active=60s idle=0
```

| Field | Meaning |
|---|---|
| `dec` | Frames decoded per second |
| `shown` | Frames shown per second (for a rotating camera roughly `dec` x active/60) |
| `dropped` | Frames skipped while the camera was visible (two ripe at once, or a full jitter buffer), with the share of decoded frames. Should stay below 1 %. |
| `late` | Frames whose target time had already passed when they left the decoder |
| `depth` | Average number of frames in the jitter buffer |
| `jitter` | p50 / p95 of arrival minus timestamp minus anchor, in ms |
| `latency` | Delay from network arrival to display, in ms |
| `active` | Seconds the camera was visible in the window (60 for a fixed camera) |
| `idle` | Frames released while the camera was not visible (not counted as dropped) |

Extra lines once a minute: `<name>: diag regulated ptsdelta ... synthetic=... leaks_closed=... leaks_active=...` (the last three should normally be 0) and a global `diag: busy_drops=... switches=...`. `diag: WARNING late1+=...` means flips landed one vblank later than intended.

**Measure only with this line.** Do not run `ffprobe` or `mpv` against the same cameras while the wall is running. Some NVRs share frames between sessions, so an extra viewer steals frames and causes visible drops.

## Troubleshooting

Common problems, with fixes, are in [docs/troubleshooting.md](docs/troubleshooting.md): `gpu_mem` and `REQBUFS OUTPUT: Invalid argument`, black screen and wrong connector, a desktop holding DRM master, H.265 cameras, too many streams, choppy video, and `scripts/reconnect-test.sh`.

## Building from source

```bash
sudo apt install build-essential pkg-config libdrm-dev libavformat-dev libavcodec-dev libavutil-dev
make                 # builds src/rpi4-rtsp (-Werror)
make test            # unit tests for the pure logic, no Pi hardware needed
make test SANITIZE=1 # with AddressSanitizer and UBSan (off by default on the Pi, where ASan may not start)
make WERROR=0        # packagers: build without -Werror (warnings stay on)
```

## Roadmap

These are ideas, not promises.

- H.265 via the stateless `rpivid` decoder
- Camera name overlay
- Web UI for configuration
- A software-decode path for the Pi 5 (open question whether it is worth it)
- Multi-monitor

## Contributing, security, license

- Contributions are welcome: see [CONTRIBUTING.md](CONTRIBUTING.md).
- Report vulnerabilities privately: see [SECURITY.md](SECURITY.md).
- Changes per release: [CHANGELOG.md](CHANGELOG.md).
- Licensed under the [MIT License](LICENSE).

## Acknowledgements

[displaycameras](https://github.com/Anonymousdog/displaycameras) showed how good a hardware-decoded Pi camera wall can be on the Pi 3 with omxplayer and DispmanX. rpi4-rtsp exists because that approach cannot follow the Pi to current Raspberry Pi OS, and it aims to bring the same idea to the Pi 4 through V4L2 and DRM/KMS.
