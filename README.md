# rtspwall

**Turn a Raspberry Pi 4 and any HDMI TV into a 24/7 camera wall for UniFi Protect, Reolink, Hikvision, Dahua and Frigate. Hardware-decoded, no desktop.**

[![CI](https://github.com/Hovhas/rtspwall/actions/workflows/ci.yml/badge.svg)](https://github.com/Hovhas/rtspwall/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Platform: Raspberry Pi 4](https://img.shields.io/badge/platform-Raspberry%20Pi%204-c51a4a.svg)

![Rendered illustration of a 2x2 wall with four test patterns labelled CAM 1–4 and running timecodes; the lower-right tile switches from CAM 4 to CAM 5.](docs/media/demo.gif)

<sub>Rendered illustration made from the bundled demo clips (`sudo rtspwall demo` shows the same wall on your TV). The rotation is shortened to fit the 10 s loop. Photos of real walls are welcome in [Discussions](https://github.com/Hovhas/rtspwall/discussions). [Smaller video](docs/media/demo.mp4).</sub>

<sub>Formerly named rpi4-rtsp (renamed before the first release).</sub>

## Will it work for me?

Check these four things. If one fails, follow the link.

- **Board:** a Raspberry Pi 4. The Pi 400 and CM4 should work but are untested. A Pi 5 does **not** work: it has no H.264 hardware decoder ([why, and what to use instead](docs/faq.md#can-i-use-a-raspberry-pi-5)).
- **Cameras:** they can send **H.264**. A sub-stream is fine, and usually better. Cameras that can only send H.265 are not supported yet ([options](docs/faq.md#my-cameras-only-send-h265)).
- **TV:** any HDMI TV or monitor, on either HDMI port.
- **The Pi:** it is dedicated to the wall and runs Raspberry Pi OS **Lite** 64-bit (Bookworm or Trixie), with no desktop ([why](docs/faq.md#can-i-keep-the-desktop)).

Not sure? Run the demo below first. It needs no cameras.

## Try it in 5 minutes

You need a Pi 4 with Raspberry Pi OS Lite 64-bit, a TV and an SSH login. [Getting started](docs/getting-started.md) covers flashing the card.

> **Testing the release candidate?** v0.1.0 is not out yet. Install v0.1.0-rc1 with:
>
> ```bash
> curl -fsSL https://github.com/Hovhas/rtspwall/releases/download/v0.1.0-rc1/get.sh | sudo RTSPWALL_VERSION=v0.1.0-rc1 bash
> ```
>
> Reports are welcome through the [issue forms](https://github.com/Hovhas/rtspwall/issues/new/choose), especially the camera report.

1. **Install** (about 2 to 4 minutes). It checks your board and OS, verifies the download, installs the package, and never reboots or changes `gpu_mem`:

   ```bash
   curl -fsSL https://github.com/Hovhas/rtspwall/releases/latest/download/get.sh | sudo bash
   ```

2. **Demo** (about 1 minute). Four moving test tiles appear on the TV. Press Ctrl-C to stop.

   ```bash
   sudo rtspwall demo
   ```

3. **Probe** your first camera. You paste the URL at a hidden prompt, so it stays out of your shell history:

   ```bash
   sudo rtspwall probe
   ```

4. **Add** it. The camera appears on the wall and starts at every boot:

   ```bash
   sudo rtspwall add front-door
   ```

After adding cameras, run `sudo rtspwall doctor`. It names any problem, says if `gpu_mem` must be raised, and prints the fix.

<details><summary>Other ways to install</summary>

- **Download, inspect, run:** `curl -fsSLO https://github.com/Hovhas/rtspwall/releases/latest/download/get.sh`, read it, then `sudo bash get.sh`.
- **Plain package:** `wget https://github.com/Hovhas/rtspwall/releases/latest/download/rtspwall_trixie_arm64.deb && sudo apt install ./rtspwall_trixie_arm64.deb` (use `bookworm` in both places on Bookworm).
- **From source:** see [CONTRIBUTING.md](CONTRIBUTING.md).
- **Upgrade:** run `get.sh` again. **Remove:** `sudo apt remove rtspwall` (`purge` also deletes your config).
- **Verify a download:** see [SECURITY.md](SECURITY.md#verifying-a-release).

</details>

## Why rtspwall

Every number below comes from one measured setup (build 0.1.0-rc2-dev, 2026-10-03): **4 UniFi Protect cameras over `rtsps` (H.264, 1024x576, three at 25 fps and one at 30 fps) on a Pi 4 with 8 GB, Raspberry Pi OS Trixie 64-bit, wired Ethernet, one 1920x1080 60 Hz display, `GRID=2x2`, default `BUFFER_MS=160`.** Method and gaps are in [docs/benchmarks.md](docs/benchmarks.md).

- **Smooth.** Each camera has its own jitter buffer. In eight consecutive one-minute lines per camera, 0.0 to 0.3 % of frames were dropped. The one 30 fps camera needed `delay_ms=40` for that ([why](docs/troubleshooting.md#the-picture-is-choppy)).
- **Live.** About 150 ms from the network to the screen (149 to 156 ms for the 25 fps cameras), and about 185 ms (177 to 189 ms) for the camera with the extra 40 ms delay.
- **Recovers by itself.** After the NVR was blocked for 15 s (three runs), the cameras reconnected 1 to 2 s after the block ended and the picture was back within 2 to 6 s.
- **Light on the CPU.** The video decoder and the display's scaler do the pixel work. The rtspwall process used 2.2 % of all four cores (8.8 % of one core) over 60 s, with no throttling (`vcgencmd get_throttled` read `0x0`).
- **Plain Debian.** Packages for Bookworm and Trixie that use the stock FFmpeg, under the MIT licence. Only Trixie is tested on hardware so far ([compatibility](docs/compatibility.md)).

Not measured yet, so not promised: how many days in a row it runs without a restart.

## How it compares

Fair and short. The [full table with sources](docs/comparison.md) says where each alternative is the better choice.

| | rtspwall | [viewpie](https://github.com/snoack/viewpie) | Old walls (displaycameras, rpisurv, camplayer) |
|---|---|---|---|
| Boards | Pi 4 | Pi 3, 4 and 5 | Written for older boards and OS images |
| H.265 | No | Yes, in hardware on Pi 4 and 5 | No |
| Current Pi OS | Bookworm and Trixie | Trixie (Bookworm only via Docker) | No: need Buster and omxplayer |
| Smoothness and latency numbers | Published | Not published | Not published |
| TV remote (CEC), two screens | No | Yes | No |
| Licence | MIT | GPL-3.0 | Apache-2.0 or GPL-2.0 |

Coming from displaycameras, rpisurv or camplayer? Read [Migrating](docs/migrating.md).

## Documentation

| I want to... | Read |
|---|---|
| Go from a blank SD card to a working wall | [Getting started](docs/getting-started.md) |
| Connect UniFi, Reolink, Hikvision, Dahua, Axis or Frigate | [Cameras](docs/cameras.md) |
| Change the layout, rotation or display mode | [Configuration](docs/configuration.md) |
| Fix a problem | [Troubleshooting](docs/troubleshooting.md), [FAQ](docs/faq.md) |
| See what is tested | [Compatibility](docs/compatibility.md), [Benchmarks](docs/benchmarks.md) |
| Understand how it works | [How it works](docs/how-it-works.md), [Glossary](docs/glossary.md) |

## Contributing, security, licence

- Questions and show-your-wall posts: [GitHub Discussions](https://github.com/Hovhas/rtspwall/discussions).
- Bugs and camera reports: the [issue forms](https://github.com/Hovhas/rtspwall/issues/new/choose). Contributing: [CONTRIBUTING.md](CONTRIBUTING.md).
- Vulnerabilities: report them privately, see [SECURITY.md](SECURITY.md).
- Changes per release: [CHANGELOG.md](CHANGELOG.md). Licence: [MIT](LICENSE).

[displaycameras](https://github.com/Anonymousdog/displaycameras) showed how good a hardware-decoded Pi camera wall can be. rtspwall brings the same idea to the Pi 4 on current Raspberry Pi OS.
