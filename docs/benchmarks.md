# Benchmarks

The numbers we have, with the conditions they were measured under, and a list of what is still missing. Read every number together with its conditions. A number without them means nothing.

## The reference setup

| | |
|---|---|
| Board | Raspberry Pi 4, 8 GB |
| OS | Raspberry Pi OS Trixie, 64-bit |
| Cameras | 6 UniFi Protect cameras, Medium stream, H.264, 1024x576 at 25 to 30 fps |
| Display | One display at 1920x1080, 60 Hz |
| Layout | Six cameras on four tiles, two of the tiles rotating |
| Settings | Defaults (`BUFFER_MS=160`) |

## Results

| What | Result | How it was read |
|---|---|---|
| Dropped frames | 0.0 to 0.2 % | The `dropped=` share in the [60 second line](configuration.md#the-60-second-statistics-line) |
| Latency | About 150 ms | The `latency=` field: time from the frame arriving from the network to it being on the screen. It does **not** include the camera's own encoding time or the camera-to-screen glass delay |
| Recovery | 4 to 6 seconds | All cameras were blocked for 15 s with `scripts/reconnect-test.sh`. This is the time from the end of the block until the picture came back |
| CPU of the rtspwall process | 2 to 13 % in `top`, typically 6 to 9 % | Read in `top`. We did not record whether this scale is a share of one core or of all four, so treat it as "low single-digit to low double-digit percent" until we measure it properly |

The load of this setup on the decoder is 79.4 % by the model in [Configuration](configuration.md#decoder-budget). That is a calculation, not a measurement.

### How the default buffer was chosen

On cameras we tested, `BUFFER_MS=120` gave 0.7 to 0.9 % late frames at 30 fps (about 9 % at 24 fps). `BUFFER_MS=160` gave 0 to 1 late frames per minute, for about 40 ms more latency. Your cameras may differ. See [Troubleshooting](troubleshooting.md#the-picture-is-choppy).

### What the jitter buffer fixed

On one NVR that stamps 30 fps frames in pairs, dropped frames reached up to 50 % before the PLL was added. [Architecture](architecture.md) explains how.

## Method

- Statistics come from the line that every camera logs once a minute. Read it with `journalctl -u rtspwall | grep ': 60s'`.
- Do not run `ffprobe`, `mpv` or VLC against the same cameras while measuring. Some NVRs share frames between viewers, so a second viewer steals frames.
- Recovery: `sudo ./scripts/reconnect-test.sh HOST PORT 15`, then watch the wall.
- A model-based decoder load comes from `sudo rtspwall probe`. It is not a measurement of the real decoder.

## What is missing

These have not been measured. We do not claim them anywhere.

- CPU as a share of all four cores, with a stated scale.
- Temperature and throttling (`vcgencmd get_throttled`) during a long run.
- How long the wall runs without a restart, and whether latency drifts over days. We say "tested for N hours" only when a run of N hours is documented here.
- Recovery after a 5 minute outage, an NVR reboot, and a power cut.
- Bookworm, a Pi 4 with 2 GB, a 4K TV, and other camera brands.
- A baseline of another player (for example mpv) on the same Pi.
- The real minimum `gpu_mem` for a given number of streams.
- Real end-to-end latency, filmed through the camera and the wall with a stopwatch in view.

## Add your numbers

Post your setup and numbers in [Discussions](https://github.com/Hovhas/rtspwall/discussions). Please include board and RAM, OS, kernel (`uname -r`), the codec, size and frame rate of each camera, the display mode, and the 60 second lines.
