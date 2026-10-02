# Comparison

A fair look at the alternatives, and where each one is the better choice.

**Dated 2026-10-01.** The facts come from our research on that date, which read each project's README, code and packaging. Almost none of it was tested on hardware by us. Projects change quickly, especially viewpie, which was released the day before. **Corrections are welcome**: open a [Discussion](https://github.com/Hovhas/rtspwall/discussions) or a pull request.

## Which one should I use?

| If you... | Use |
|---|---|
| Have a Pi 4, H.264 cameras, and want a smooth wall that runs on Bookworm or Trixie | rtspwall |
| Have a Pi 5, or cameras that only send H.265 | [viewpie](https://github.com/snoack/viewpie) |
| Want a TV remote (CEC), two screens, or a Docker image | viewpie |
| Have a small x86 mini PC, or a wall of many cameras | [OpenSurv](https://github.com/OpenSurv/OpenSurv) |
| Want zero configuration, use only UniFi Protect, and can pay for hardware | [UniFi Protect ViewPort](https://store.ui.com/us/en/products/ufp-viewport) |
| Run an old Pi 3 image from Buster | The old tools still work there, but see [Migrating](migrating.md) |

## Pi-based camera walls

| Project | Licence | Runs on current Pi OS | How it decodes on a Pi 4 | Needs a desktop | H.265 | Pi 5 |
|---|---|---|---|---|---|---|
| **rtspwall** | MIT | Trixie tested on hardware. Bookworm: package built in CI, not yet tested on hardware | Hardware H.264 into a display plane, no copies | No | No | No |
| [viewpie](https://github.com/snoack/viewpie) | GPL-3.0 | Trixie package yes. Bookworm package does not install (needs `libavcodec61`). Docker image on both | Hardware H.264, same decoder and similar design (from its README and packaging) | No | Hardware on Pi 4 and 5 | Yes (H.264 in software) |
| [OpenSurv](https://github.com/OpenSurv/OpenSurv) | GPL-2.0 | Pi 4 on Bookworm marked "Limited Performance"; Trixie "testing phase" | mpv in X11, software by default | Yes | Via mpv, users report instability | Yes, limited |
| [PiMonitor](https://github.com/tozred/Pi-CCTV-Monitor) | MIT | Claims Bookworm and Trixie, unconfirmed | mpv per tile in Xorg, copy or software | Yes | Via mpv | Claimed |
| [displaycameras-modern](https://github.com/MaisUmGajo/displaycameras-modern) | Apache-2.0 | Author verified Pi 4 on Trixie | Xorg and mpv, software by default | Yes | Software | Claimed, untested |
| [unifi-viewport](https://github.com/jsetsuda/unifi-viewport) | MIT | Desktop image with X11 | mpv per tile, software on Pi 4 | Yes | Advises against | Software |
| [kiosk-monitor](https://github.com/extremeshok/kiosk-monitor) | MIT | Trixie desktop only | Chromium or VLC, one URL per display | Yes | Via browser | Tested platform, software |
| [PiPlay](https://github.com/HussX/piplay) | none | Bookworm Lite (main branch) | Software (OpenCV and Qt) | No (main branch) | Software | Software |
| [displaycameras](https://github.com/Anonymousdog/displaycameras) | Apache-2.0 | **No**: needs Buster and omxplayer | omxplayer | No | No | No |
| [rpisurv](https://github.com/SvenVD/rpisurv) | GPL-2.0 | **No**: needs Buster and VLC with MMAL | VLC MMAL | Buster Full image | No | No |
| [camplayer](https://github.com/raspicamplayer/camplayer) | GPL-2.0 | **No**: omxplayer, DispmanX | omxplayer | No | VLC, fullscreen only | No |

Not covered here: other projects that appeared in search (camwall, PiViewWall, PiCams, OpenViewport and more). We did not check them.

## UniFi Protect ViewPort

A hardware appliance. Zero configuration, 16-stream matrix and 4K at 30 fps output, per the product page. On the research date it was on sale at $199 (about $233 with surcharges). It shows **one Protect controller only**. rtspwall is free, works with any RTSP camera and several NVRs at once, and has no ViewPort-style live-view sync.

## rtspwall and viewpie, side by side

viewpie is the closest project. Both use the Pi's hardware decoder and put each camera on its own display plane.

**Where viewpie is better today**

- H.265 in hardware on the Pi 4 and Pi 5.
- Runs on Pi 3, 4 and 5, and has 32-bit packages.
- TV remote over HDMI-CEC, and pause when the TV is off.
- Two screens at once.
- Drawn layouts with tiles that span cells, and a Docker image.
- Its configuration checker also refuses feeds that no tile shows.

**Where rtspwall differs**

- Packages for both Bookworm and Trixie. (viewpie's package installs on Trixie only.)
- Published smoothness and latency numbers for one setup: 0.0 to 0.2 % dropped frames and about 150 ms, and a recovery of 4 to 6 seconds after a 15 second outage. viewpie's README gives CPU figures but says nothing about latency or dropped frames. See [Benchmarks](benchmarks.md) for our conditions.
- MIT licence instead of GPL-3.0.
- A decoder-budget check (`probe`) and `gpu_mem` guidance.
- A demo that needs no cameras, and a `doctor` command.

**Same on both**

- No desktop, no browser, one DRM client.
- A hardened systemd service under its own user.

## Sources

Read on 2026-10-01 unless noted. Star counts and activity change, so we left them out.

- viewpie: <https://github.com/snoack/viewpie> (README, `debian/control`, release assets)
- OpenSurv: <https://github.com/OpenSurv/OpenSurv>
- PiMonitor: <https://github.com/tozred/Pi-CCTV-Monitor>
- displaycameras-modern: <https://github.com/MaisUmGajo/displaycameras-modern>
- unifi-viewport: <https://github.com/jsetsuda/unifi-viewport>
- kiosk-monitor: <https://github.com/extremeshok/kiosk-monitor>
- PiPlay: <https://github.com/HussX/piplay>
- displaycameras, rpisurv, camplayer: <https://github.com/Anonymousdog/displaycameras>, <https://github.com/SvenVD/rpisurv>, <https://github.com/raspicamplayer/camplayer>
- UniFi Protect ViewPort: <https://store.ui.com/us/en/products/ufp-viewport>
