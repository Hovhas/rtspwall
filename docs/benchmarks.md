# Benchmarks

The numbers we have, with the conditions they were measured under, and a list of what is still missing. Read every number together with its conditions. A number without them means nothing.

## The measured setup

Measured on the maintainer's Pi on 2026-10-03, build 0.1.0-rc2-dev.

| | |
|---|---|
| Board | Raspberry Pi 4, 8 GB |
| OS | Raspberry Pi OS Trixie, 64-bit, kernel 6.18.50+rpt-rpi-v8 |
| Cameras | 4 UniFi Protect cameras through `rtsps` (`UNIFI_REWRITE=tls`, the default), H.264, 1024x576: three at 25 fps, one at 30 fps |
| Network | Wired Ethernet |
| Display | One display at 1920x1080, 60 Hz, on HDMI-A-2 |
| Layout | `GRID=2x2`, one camera per tile |
| Settings | Defaults (`BUFFER_MS=160`), except `delay_ms=40` on the 30 fps camera |

## Results

| What | Result | How it was read |
|---|---|---|
| Dropped frames | 0.0 to 0.3 % for the worst camera, 0.0 to 0.1 % for the others | The `dropped=` share in 8 consecutive [60 second lines](configuration.md#the-60-second-statistics-line) per camera (19:09 to 19:17) |
| Latency | 149 to 156 ms for the 25 fps cameras, 177 to 189 ms for the camera with `delay_ms=40` | The `latency=` field: time from the frame arriving from the network to it being on the screen. It does **not** include the camera's own encoding time or the camera-to-screen glass delay |
| Recovery | Cameras reconnected 1 to 2 s after the block ended. The picture was back within 2 to 6 s | The NVR was blocked for 15 s with `scripts/reconnect-test.sh`, three runs. The time is counted from the end of the block |
| CPU of the rtspwall process | 2.2 % of all four cores (8.8 % of one core). An earlier 60 s sample on rc1 gave 2.1 % | `utime` plus `stime` from `/proc/PID/stat`, read at the start and end of 60 s, divided by 60 s times four cores |
| Throttling | `throttled=0x0` | `vcgencmd get_throttled` |
| Decoder memory | `CmaFree` was identical after three NVR blocks. `bufs=16` stayed steady | `CmaFree` in `/proc/meminfo`, compared across the three blocks. rc1 lost about 55 MB per block under the same test |

The load of this setup on the decoder is 46.3 % by the model in [Configuration](configuration.md#decoder-budget): (2304 macroblocks x 25 fps x 3 + 2304 x 30 fps) / 522,240. That is a calculation, not a measurement.

### One 30 fps camera needed extra delay

The 30 fps camera without `delay_ms` (rc1, 151 one-minute lines): 1.2 to 1.6 % dropped in typical lines (average 1.8 %, maximum 4.6 %), `late` 41 to 63 per minute, jitter p50 35 ms and p95 about 110 ms (maximum 149 ms). The regulated `ptsdelta` was tight at 33 ms, so the camera sent evenly and the frames arrived from the network in bursts. With `delay_ms=40`: 0.0 % dropped, `late` 0 per minute, jitter p95 about 50 ms, latency about 185 ms. See [Troubleshooting](troubleshooting.md#the-picture-is-choppy).

## Earlier measurements (6 cameras, plain RTSP, rc1)

| | |
|---|---|
| Board | Raspberry Pi 4, 8 GB |
| OS | Raspberry Pi OS Trixie, 64-bit |
| Cameras | 6 UniFi Protect cameras, Medium stream, H.264, 1024x576 at 25 to 30 fps, plain RTSP rewrite |
| Display | One display at 1920x1080, 60 Hz |
| Layout | Six cameras on four tiles, two of the tiles rotating |
| Settings | Defaults (`BUFFER_MS=160`) |

| What | Result | How it was read |
|---|---|---|
| Dropped frames | 0.0 to 0.2 % | The `dropped=` share in the 60 second line |
| Latency | About 150 ms | The `latency=` field, as above |
| Recovery | 4 to 6 seconds | All cameras were blocked for 15 s with `scripts/reconnect-test.sh`. This is the time from the end of the block until the picture came back |
| CPU of the rtspwall process | 2 to 13 % in `top`, typically 6 to 9 % | Read in `top`. The scale (one core or all four) was not recorded, so this is not comparable with the CPU figure above |

The load of this setup on the decoder is 79.4 % by the model if all six cameras run at 30 fps. It was lower, because some ran at 25 fps. That is a calculation, not a measurement.

### How the default buffer was chosen

On cameras we tested, `BUFFER_MS=120` gave 0.7 to 0.9 % late frames at 30 fps (about 9 % at 24 fps). `BUFFER_MS=160` gave 0 to 1 late frames per minute, for about 40 ms more latency. Your cameras may differ. See [Troubleshooting](troubleshooting.md#the-picture-is-choppy).

### What the jitter buffer fixed

On one NVR that stamps 30 fps frames in pairs, dropped frames reached up to 50 % before the PLL was added. [Architecture](architecture.md) explains how.

## Method

- Statistics come from the line that every camera logs once a minute. Read it with `journalctl -u rtspwall | grep ': 60s'`.
- Do not run `ffprobe`, `mpv` or VLC against the same cameras while measuring. Some NVRs share frames between viewers, so a second viewer steals frames.
- Recovery: `sudo ./scripts/reconnect-test.sh HOST PORT 15`, then watch the wall.
- CPU: read `utime` and `stime` (fields 14 and 15, in clock ticks) of the rtspwall process in `/proc/PID/stat` at the start and the end of 60 s. Divide the sum by 60 s times the clock ticks per second (`getconf CLK_TCK`). The result is a share of one core. Divide by 4 for the share of all four cores.
- Decoder memory: compare `CmaFree` in `/proc/meminfo` before and after an outage, and watch `bufs=` in the `diag regulated ptsdelta` line.
- A model-based decoder load comes from `sudo rtspwall probe`. It is not a measurement of the real decoder.

## What is missing

These have not been measured. We do not claim them anywhere.

- Temperature and throttling (`vcgencmd get_throttled`) during a long run. Only a 60 s reading (`0x0`) exists.
- How long the wall runs without a restart, and whether latency drifts over days. We say "tested for N hours" only when a run of N hours is documented here.
- Recovery after a 5 minute outage, an NVR reboot, and a power cut.
- Bookworm, a Pi 4 with 2 GB, a 4K TV, and other camera brands. All numbers above are UniFi Protect on Trixie with a 1080p60 display.
- A baseline of another player (for example mpv) on the same Pi.
- The real minimum `gpu_mem` for a given number of streams.
- Real end-to-end latency, filmed through the camera and the wall with a stopwatch in view.

## Add your numbers

Post your setup and numbers in [Discussions](https://github.com/Hovhas/rtspwall/discussions). Please include board and RAM, OS, kernel (`uname -r`), the codec, size and frame rate of each camera, the display mode, and the 60 second lines.
