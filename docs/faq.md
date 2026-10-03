# FAQ

Straight answers, including the ones that are not what you hoped for.

## Can I use a Raspberry Pi 5?

No, not today. The Pi 5 has no H.264 hardware decoder. rtspwall is built around the Pi 4's decoder: the decoder writes each frame and the display reads it, with no copy in between. On a Pi 5 the H.264 work would fall back to the CPU, and that breaks the idea. `rtspwall doctor` says so, and the service stops with a clear message instead of looping.

If you only have a Pi 5, look at [viewpie](https://github.com/snoack/viewpie). According to its README it supports Pi 3, 4 and 5, decodes H.265 in hardware on the Pi 4 and 5, and has a TV-remote and two-screen mode. We have not run it ourselves. See the [comparison](comparison.md).

A software-decode path for the Pi 5 is an open question, not a promise.

## My cameras only send H.265

rtspwall shows H.264 only. You have three options:

1. Switch the camera, or its sub-stream, to H.264. Most cameras allow this. See [Cameras](cameras.md#switching-a-camera-to-h264-or-a-sub-stream). UniFi: turn off Enhanced encoding.
2. Let go2rtc or Frigate convert the stream. This costs CPU on the NVR machine, not on the Pi. See [Cameras](cameras.md#turning-h265-into-h264-with-go2rtc).
3. Use a different tool that decodes H.265, for example viewpie on a Pi 4 or 5.

H.265 support for the Pi 4 is on the list of ideas. It needs a research step first, so there is no date.

## Can I keep the desktop?

Not on the same screen. Only one program can drive a screen at a time ([DRM master](glossary.md#drm-master)). A desktop takes that role, and rtspwall needs it. Use Raspberry Pi OS **Lite**. A text login on the console does not get in the way.

If you have a desktop image, `rtspwall doctor` finds the program that holds the screen and prints the fix: `sudo raspi-config nonint do_boot_behaviour B1`, then a reboot. After that the Pi boots to the console and the wall can take the screen.

## How do I get my console back?

`sudo systemctl stop rtspwall`. See [Getting started](getting-started.md#getting-your-console-back).

## How many cameras can I show?

The limit is the decoder, not the screen. The Pi 4 decoder is specified for about 1080p60 in total. `rtspwall probe` computes your total against that budget. Four 1024x576 streams (three at 25 fps, one at 30 fps) use about 46 % of it. That setup ran on a Pi 4 with 4 UniFi cameras. The earlier reference setup, six 1024x576 streams at 25 to 30 fps, was calculated at 79 % when all six run at 30 fps. Four 1080p streams at 15 fps are calculated at about 94 %. The numbers and the method are in [Configuration](configuration.md#decoder-budget). The software maximum is 16 cameras.

Cameras in a rotation group all decode all the time, so rotation does not raise the number of cameras you can run. Use sub-streams.

## Can I use the second HDMI port or two screens?

You can plug the TV into either port. Driving two screens at once is not supported. Both screens would share the same planes and decoder budget.

## Does Wi-Fi work?

Yes, but Ethernet is better for video. If you use Wi-Fi, turn Wi-Fi power save off, because it causes stalls and late frames. `rtspwall doctor` checks this. The command it suggests is `sudo iw dev wlan0 set power_save off` (until reboot) or a permanent setting with `nmcli`.

## Does it record, or play audio?

No. Recording and audio are not goals of this project. Pair it with an NVR such as UniFi Protect or Frigate for recording.

## Why not VLC, mpv or a browser?

They work, and are a fine choice for one or two cameras. On a Pi 4 they usually decode in software or copy each frame, and they need a desktop. rtspwall is for a fixed wall that runs for days without a keyboard. We have not measured them on the same Pi yet, so we make no speed claim ([Benchmarks](benchmarks.md)).

## Why not a mini PC, a Fire TV or an Apple TV?

- **Mini PC.** An x86 mini PC decodes H.265, drives more cameras and can run other software. It also costs more and uses more power than a Pi 4 you may already have. If you have one, use it. OpenSurv is a project that is strong on small x86 PCs ([comparison](comparison.md)).
- **Fire TV or Apple TV.** We have not tested these. They generally run vendor apps, which usually show one brand at a time and give you less control over layout and rotation.
- **UniFi ViewPort.** It is zero-configuration and works with UniFi Protect. It shows one Protect controller only, and on the date of our research it was listed at $199 (about $233 with surcharges). rtspwall works with any RTSP camera, and with several NVRs at once, but it has no live-view sync with Protect.

## Will the Pi 4 stay available?

Raspberry Pi has published a long production commitment for the Pi 4. We have not re-checked the current date. Look at the Raspberry Pi product page.

## Is it a UniFi ViewPort replacement?

For showing cameras on a TV: yes, if you accept the limits above. It does not copy ViewPort's single-click Protect features.

## Is my traffic encrypted?

For most cameras, no. Plain `rtsp://` URLs are unencrypted traffic on your network. For UniFi Protect, rtspwall keeps the encrypted `rtsps` URL by default (`UNIFI_REWRITE=tls`). That stops someone who only listens to the network. It does not stop someone who pretends to be the camera, because FFmpeg does not verify the certificate. `UNIFI_REWRITE=plain` sends the token and the video unencrypted, and `doctor` warns about it. Read the [security notes](../SECURITY.md#the-unifi-tls-trade-off) and put the cameras on an isolated network or VLAN where you can.

## Why does `probe` refuse my URL?

A URL with a password, a query string or a token on the command line is saved in your shell history, shown by `ps` and logged by `sudo`. Run `sudo rtspwall probe` and paste the URL at the hidden prompt, or use `sudo rtspwall probe - < url.txt`. The flag `--insecure-argv` allows it anyway. See [Cameras](cameras.md).

## Do I need to run `doctor` after the install?

Not right away. The installer does not run it and does not change `gpu_mem`. Add your cameras first, then run `sudo rtspwall doctor`. It tells you if `gpu_mem` must be raised, and `sudo rtspwall doctor --fix` can do it after asking.

## Where are my passwords stored?

In `/etc/rtspwall/cameras.conf`, readable by `root` and the `rtspwall` service only. Logs and `doctor --report` mask them on a best-effort basis. See [SECURITY.md](../SECURITY.md).

## I still have a question

Ask in [GitHub Discussions](https://github.com/Hovhas/rtspwall/discussions). Bugs go to the [issue forms](https://github.com/Hovhas/rtspwall/issues/new/choose).
