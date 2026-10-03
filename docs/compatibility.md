# Compatibility

What has been tested, and what has not. **Last updated: 2026-10-03.**

This page lists only things somebody ran. Everything else says **community reports welcome**. If you tried something, please tell us with the [camera report form](https://github.com/Hovhas/rtspwall/issues/new?template=camera-report.yml), or show your wall in [Discussions](https://github.com/Hovhas/rtspwall/discussions). Add the output of `sudo rtspwall doctor --report`, and read it before you paste it.

## Boards and operating systems

| Board | OS | Kernel | Result | Tested by |
|---|---|---|---|---|
| Pi 4, 8 GB | Raspberry Pi OS Trixie, 64-bit | 6.18.50+rpt-rpi-v8 | Works: the reference setup in [Benchmarks](benchmarks.md) | Project maintainer |
| Pi 4, 8 GB | Raspberry Pi OS Bookworm, 64-bit | not tested | Package is built in CI. **Not tested on hardware.** Community reports welcome | |
| Pi 4, 2 GB or 4 GB | any | not tested | Community reports welcome | |
| Pi 400, CM4 | any | not tested | Same chip as the Pi 4, so it should work. Community reports welcome | |
| Pi 5 | any | | Does not work: no H.264 hardware decoder ([FAQ](faq.md#can-i-use-a-raspberry-pi-5)) | |
| Pi 3 and older | any | | Not supported | |
| Raspberry Pi OS Desktop | any | | Does not work on the same screen ([FAQ](faq.md#can-i-keep-the-desktop)) | |

## Cameras and NVRs

| Brand or system | Model and firmware | Stream | Result | Reporter |
|---|---|---|---|---|
| UniFi Protect | 6 cameras, console and firmware not recorded | Medium stream, H.264, 1024x576 at 25 to 30 fps, through the UniFi URL rewrite to plain RTSP (the default at the time; the default is now `rtsps` kept, see [Cameras](cameras.md#unifi-protect)) | Works with the plain rewrite | Project maintainer |
| UniFi Protect | 4 cameras, console and firmware not recorded | H.264, 1024x576 at 25 and 30 fps, `UNIFI_REWRITE=tls` (default: `rtsps` on port 7441 kept, `?enableSrtp` removed). rtspwall 0.1.0-rc1 | Works: all four connect and play. Stable for hours; no long soak test measured | Project maintainer |
| Reolink | | | Community reports welcome | |
| Hikvision, ABUS | | | Community reports welcome | |
| Dahua, Amcrest | | | Community reports welcome | |
| Axis | | | Community reports welcome | |
| Frigate or go2rtc restream | | | Community reports welcome | |
| Any H.265-only stream | | | Not supported yet | |

## TVs and monitors

| Display | Mode | Result |
|---|---|---|
| One 1080p display at 60 Hz | 1920x1080 at 60 Hz | Works (the reference setup) |
| TV on HDMI-A-2 (HDMI1, the port away from the USB-C power port) | 1920x1080 at 60 Hz, `MODE=auto` picked the preferred mode | Works |
| 4K TV | `MODE=auto` should pick 1080p at 60 Hz | Not tested on hardware. Community reports welcome |
| TV standby, input switch, unplugged cable while running | | Built to recover by itself. Not tested on hardware with several TV brands. Community reports welcome |
| Ultrawide or 1440p monitor | Preferred mode is kept | Not tested. Community reports welcome |

## How to add a row

Open the [camera report form](https://github.com/Hovhas/rtspwall/issues/new?template=camera-report.yml) and fill in the brand, model, firmware, stream codec, resolution, frame rate, how many cameras, and the result. We add confirmed reports here with the date.
