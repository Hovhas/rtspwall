# Configuration

Reference for the config file and the command line. For a first setup, read [Getting started](getting-started.md) instead.

## The config file

- Location: `/etc/rtspwall/cameras.conf`.
- Permissions: `0640`, owner `root`, group `rtspwall`. The file holds camera passwords. Do not use `chmod 600`: it locks the service out. Keep the file out of git.
- Edit it with `sudoedit /etc/rtspwall/cameras.conf`.
- Only root may change the file. `sudo rtspwall doctor` reports a `FAIL` if the owner (other than root), the group or other users can write to it.
- When you save a **valid** file and the wall is enabled, a watcher (`rtspwall-config.path`) checks it as the user `rtspwall` and restarts the wall within a few seconds. This also restarts a wall that had stopped with `failed` because of an earlier bad file. Three cases change nothing: while `rtspwall demo` runs, a wall you stopped on purpose stays stopped, and a file that fails the check leaves the running wall untouched. The last case logs one line: `cameras.conf changed but is INVALID (or unreadable by user rtspwall): rtspwall left unchanged. Run: sudo rtspwall --check-config /etc/rtspwall/cameras.conf`
- Lines that start with `#` are comments. Settings are `KEY=VALUE` lines and can appear anywhere. A camera is one line with fields separated by `|`.

A commented example is installed with the package and lives in the repository as [examples/cameras.conf](../examples/cameras.conf).

Check a file at any time, without touching the screen or the network:

```bash
sudo rtspwall --check-config /etc/rtspwall/cameras.conf
```

`--check-config` is strict: it rejects leftover placeholders (`CHANGE_ME`, `<...>`) and unknown keys, and it suggests the nearest key (`did you mean CONNECTOR?`). The running service only warns about unknown keys, so an upgrade never stops a working wall.

## Settings

| Key | Default | What it does |
|---|---|---|
| `GRID` | not set | `COLSxROWS`, up to `8x8`. Splits the screen into equal cells. Not set means manual layout. |
| `BUFFER_MS` | `160` | How far ahead of the stream's own timestamps each frame is shown. Higher is smoother but adds latency. Lower is snappier but more late frames are skipped. |
| `ROTATE_SECONDS` | `15` | How long each camera of a rotation group is shown. |
| `MODE` | `auto` | Display mode: `auto`, `WxH` or `WxH@Hz` (for example `1280x720@50`). `auto` keeps the display's preferred mode, except that a 4K mode or one under 50 Hz is replaced by the best mode up to 1920 wide with the same shape. A 4K TV then runs at 1920x1080 at 60 Hz. Interlaced modes are never used. If the display lacks the mode, the log lists the modes it has, the wall runs with `auto`, and it switches to your `MODE` as soon as the display offers it (it checks every 30 s). |
| `UNIFI_REWRITE` | `tls` | What to do with a UniFi Protect `rtsps://HOST:7441/TOKEN?enableSrtp` URL. `tls` keeps `rtsps` on port 7441 and only removes `?enableSrtp`. `plain` rewrites it to `rtsp://HOST:7447/TOKEN`: the token and the video are then **unencrypted**, and a warning is logged. `off` uses the URL exactly as written. `auto` is an older name for `tls`. See [Cameras](cameras.md#unifi-protect) and [SECURITY.md](../SECURITY.md#the-unifi-tls-trade-off). |
| `DECODER` | `/dev/video10` | The V4L2 H.264 decoder device (`bcm2835-codec` on a Pi 4). |
| `CONNECTOR` | first connected | The display output by its kernel name, for example `HDMI-A-1` or `HDMI-A-2`. When unset, the wall starts on the first connected HDMI, DVI or DisplayPort output (it waits for one rather than starting on DSI or composite), and follows the cable: if its output has been disconnected for 10 s and another HDMI/DVI/DP output is connected, it moves there, and it moves back once the original output has been connected again for 10 s. **With two screens connected, set this** to pin the wall to one of them. |
| `DRM_DEVICE` | auto | The DRM card, for example `/dev/dri/card1`. Auto picks the first card with a connected display (or, with `CONNECTOR` set, the first card that has that connector). |
| `FFMPEG_LOGLEVEL` | `error` | How much of FFmpeg's own log reaches the journal: `quiet`, `error`, `warning` or `info`. Use `warning` to see RTP packet loss. URLs in these lines are masked. |

## Camera lines

You can use grid lines or manual lines, not both in one file. At most 16 cameras.

Supported sources: `rtsp://` and `rtsps://` URLs, and local video files. Any other URL scheme (http, rtmp, udp, ...) is rejected with `line N: unsupported URL for camera NAME`. Network sources must be H.264. See [SECURITY.md](../SECURITY.md#the-probe-sandbox).

### Grid layout

```
GRID=2x2
name|url|cell[|delay_ms]
```

Cells are numbered from 1, row by row:

```
+-----+-----+        +-----+-----+-----+
|  1  |  2  |        |  1  |  2  |  3  |
+-----+-----+        +-----+-----+-----+
|  3  |  4  |        |  4  |  5  |  6  |
+-----+-----+        +-----+-----+-----+
   GRID=2x2          |  7  |  8  |  9  |
                     +-----+-----+-----+
                        GRID=3x3
```

Tile edges sit at `floor(i * width / cols)`, so tiles cover the screen with no gaps. If the size does not divide evenly, tiles differ by at most one pixel.

`delay_ms` is optional. It adds extra delay (-10000 to 10000) for one camera on top of `BUFFER_MS`, for example when its network path is slower than the others.

### Rotation

Cameras that share a cell form a **rotation group**. They take turns, `ROTATE_SECONDS` each, and the first one listed starts visible. All cameras of a group keep decoding, so the switch has no black frame. A camera with no fresh frame for more than 3 seconds is skipped.

```
GRID=2x2
ROTATE_SECONDS=15

front-door|rtsp://viewer:PASSWORD@192.168.1.10:554/stream2|1
driveway  |rtsp://viewer:PASSWORD@192.168.1.11:554/stream2|2
garage    |rtsp://viewer:PASSWORD@192.168.1.12:554/stream2|3
side-gate |rtsp://viewer:PASSWORD@192.168.1.14:554/stream2|3
```

Rotation saves screen space, not decoder capacity. Every camera needs its own decoder instance and display plane all the time, shown or not.

### Manual layout

Leave out `GRID` and give each camera a tile in screen pixels:

```
name|url|width|height|x|y[|delay_ms]
```

The tile is the size on screen. The display hardware scales the stream. Cameras with identical `width|height|x|y` form a rotation group. A tile that reaches outside the screen gives a warning. At start, if the screen is smaller than the tiles need (a TV in standby often offers only small modes), the wall waits up to 60 s for a larger mode, then starts anyway and cuts off what does not fit.

```
front-door|rtsp://viewer:PASSWORD@192.168.1.10:554/stream2|960|540|0|0
driveway  |rtsp://viewer:PASSWORD@192.168.1.11:554/stream2|960|540|960|0
```

## Decoder budget

The Pi 4's H.264 decoder has a fixed budget. `probe` and `add` use this model: count macroblocks per second (16x16-pixel blocks times frames per second) for all cameras, rotation members included, and compare the total with H.264 level 4.2 (522,240 per second, 100 %).

| Verdict | Total load |
|---|---|
| `PASS` | up to 90 % |
| `WARN` | above 90 % and up to 100 %: works, with little headroom |
| `FAIL` | above 100 %: too much, the decoder can wedge until a reboot |

Calculated examples from the model:

| Cameras | Load | Verdict | Run on hardware? |
|---|---|---|---|
| 3 x 1024x576 at 25 fps and 1 x 1024x576 at 30 fps | 46.3 % | `PASS` | Yes, 4 UniFi cameras over `rtsps` on a Pi 4 (see [Benchmarks](benchmarks.md)) |
| 6 x 1024x576 at 30 fps | 79.4 % | `PASS` | Yes, the earlier reference setup: 6 cameras at 25 to 30 fps over plain RTSP, so its real load was a little lower than this example (see [Benchmarks](benchmarks.md#earlier-measurements-6-cameras-plain-rtsp-rc1)) |
| 4 x 1920x1080 at 15 fps | 93.8 % | `WARN` | No, calculated only |
| 6 x 1280x720 at 30 fps | 124 % | `FAIL` | No, calculated only |

`sudo rtspwall probe /etc/rtspwall/cameras.conf` probes every camera in a file and prints the table with the total.

## Commands

All commands that touch the system need `sudo`.

| Command | What it does |
|---|---|
| `rtspwall [CONFIG]` | Run the wall. This is what the service does. The default config is `/etc/rtspwall/cameras.conf`. |
| `rtspwall --check-config [--mode WxH] [CONFIG]` | Validate a config and print the layout. `--mode` sets the screen size to lay out for (default `1920x1080`). |
| `rtspwall --help`, `rtspwall --version` | Help and version. |
| `rtspwall probe [--insecure-argv] [URL \| - \| CONFIG \| FILE]` | Check a stream without playing it. No argument: hidden prompt. `-`: read the URL from stdin. A config file: probe every camera. A local video file: probe the file. A URL with a password, query string or token on the command line is **refused**, because the command line ends up in shell history, `ps` and the `sudo` log. `--insecure-argv` allows it anyway. As root, each probe runs as the user `rtspwall` in a sandbox. Only regular files are probed. Exit status: 0 `PASS`, 1 `WARN`, 2 `FAIL` or error. |
| `rtspwall add [--config PATH] [--force] [--no-systemd] NAME [CELL]` | Probe a camera, add it to the first free cell (or `CELL`), then start or restart the service. A taken `CELL` makes a rotation group. `--force` adds despite a `FAIL` or too little `gpu_mem`. `--no-systemd` only edits the file. Needs a `GRID=` line and grid-style config. See [How `add` edits the file](#how-add-edits-the-file). |
| `rtspwall doctor [--config PATH] [--fix] [--yes] [--report] [--summary] [--no-hardware] [--no-probe]` | Self-test with one `PASS`, `INFO`, `WARN` or `FAIL` line per check and a fix to copy. `--fix` offers to set `gpu_mem=256` (asks first, backs up `config.txt`, prints the command to undo it, never reboots). `--yes` answers yes without a terminal. `--report` prints one block for bug reports with URLs and tokens masked. `--summary` prints only problems and totals. `--no-hardware` skips checks that need the Pi. `--no-probe` does not contact the cameras. Exit status: 0 all pass, 1 warnings, 2 failures. See [What `doctor` checks](#what-doctor-checks). |
| `rtspwall demo` | Show the built-in 2x2 demo wall. Ctrl-C stops it and restores the previous state. |

### How `add` edits the file

- If the config path is a symlink, `add` edits the file it points to.
- It takes a lock, so a second `rtspwall add` waits until the first one is done.
- If the config changed while it probed the camera, it writes nothing and tells you to run `add` again.
- It writes a temporary file next to the config. Ctrl-C removes that file, because it holds the camera password.
- It refuses to run if the config's directory is writable by other users.
- Before it writes, it checks [`gpu_mem`](glossary.md#gpu_mem) for all cameras in the new config. If the active value is too low, it adds nothing. Too little `gpu_mem` can lock up the decoder until a reboot. It tells you to run `sudo rtspwall doctor --fix` and then reboot, or to use the camera's sub-stream. `--force` adds the camera anyway. If it cannot read the active value (`vcgencmd` is missing), it only warns.
- After the change, the watcher `rtspwall-config.path` restarts the wall when it is active. Otherwise `add` starts or restarts the service itself.

### What `doctor` checks

- **Config permissions:** `FAIL` if the config is writable by anyone but root.
- **UniFi:** `WARN` for each camera that uses `UNIFI_REWRITE=plain`. The warning is stronger when the default route goes over Wi-Fi (`wlan`).
- **`gpu_mem`:** the cameras need 256 MB when they use more than half the decoder budget, or when 4 or more of them send 1080p or larger. The second rule also holds when the frame rate is unknown. It understands `gpu_mem_1024`, which overrides `gpu_mem` on a Pi 4, and the section filters in `config.txt`. If a `gpu_mem` line sits under a filter doctor cannot evaluate (for example `[HDMI:0]` or `[board-type=...]`), it gives a `WARN` instead of a guess. `doctor --fix` then changes nothing and tells you to set the value by hand under `[all]`.

### Exit codes of the wall

| Code | Meaning | The service |
|---|---|---|
| `2` | Config error, no cameras configured, or bad command line | Not restarted. Fix the file. |
| `3` | No usable H.264 hardware decoder: `/dev/video10` is still missing after 15 s, or is not an H.264 decoder (for example a Pi 5) | Not restarted. Unsupported hardware. |
| other non-zero | A crash or failure. A failed `VIDIOC_QUERYCAP` on the decoder counts here (exit `1`). | Restarted with a growing delay. |

Before the wall gives up it waits for things that are only late at boot: the `DRM_DEVICE` node, a display, a `MODE` the display offers, a screen big enough for manual tiles (60 s, then it starts anyway), and access to the decoder (up to 30 s while udev sets its permissions).

## The 60 second statistics line

Every 60 seconds each camera logs one line. Read them with `journalctl -u rtspwall | grep ': 60s'`.

```
front-door: 60s dec=25.0 shown=25.0 dropped=0(0.0%) late=0 depth=4.1 jitter p50=3 p95=12ms latency=150ms active=60s idle=0
```

| Field | Meaning |
|---|---|
| `dec` | Frames decoded per second. |
| `shown` | Frames shown per second. For a rotating camera, about `dec` times active/60. |
| `dropped` | Frames skipped while the camera was visible, with the share of decoded frames. Keep it under 1 %. |
| `late` | Frames whose show time had already passed when they left the decoder. |
| `depth` | Average number of frames in the [jitter buffer](glossary.md#jitter-buffer). |
| `jitter` | p50 and p95 of how unevenly frames arrive, in ms. |
| `latency` | Delay from network arrival to the screen, in ms. |
| `active` | Seconds the camera was visible in the window (60 for a fixed camera). |
| `idle` | Frames released while the camera was not visible. Not counted as dropped. |

Once a minute there are also diagnostic lines:

| Line | Meaning |
|---|---|
| `NAME: diag regulated ptsdelta p5=...ms p50=...ms p95=...ms synthetic=N leaks_closed=N leaks_active=N bufs=N` | How even the frame timing is after correction (`p5`, `p50`, `p95`). `synthetic`, `leaks_closed` and `leaks_active` should normally be 0. `bufs` is the number of decoder buffers the camera holds now: usually 16 while the camera plays, plus `leaks_active`. It must not grow from one reconnect to the next. |
| `diag: busy_drops=N switches=N` | Since the last line: frames that were never shown because a display update (atomic commit) failed, and rotation switches. `busy_drops` should normally be 0. |
| `diag: WARNING late1+=N flips confirmed >=1 vblank after target (regression?)` | Picture updates landed one display refresh later than planned. |

**Measure with this line only.** Do not run `ffprobe` or `mpv` against the same cameras while the wall runs. Some NVRs share frames between viewers, so a second viewer steals frames and causes visible drops.
