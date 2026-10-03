# Glossary

Short answers for the technical words in these docs. Pages link here the first time they use a word.

## Codec

The way a video is compressed. The camera compresses, the Pi unpacks. See [H.264 and H.265](#h264-and-h265).

## CMA

Contiguous Memory Allocator. Memory that Linux sets aside for devices that need one solid block. Not the same as [`gpu_mem`](#gpu_mem), which is set in `config.txt`.

## DRM master

The one program that is allowed to control a screen. Only one program can hold it at a time. A desktop takes it, so on a Pi with a desktop, rtspwall cannot show anything. `rtspwall doctor` names the program that holds it.

## go2rtc

A small program that takes camera streams and offers them again under simple URLs, and can convert them. Frigate includes it. You can point rtspwall at its restream, usually `rtsp://HOST:8554/NAME`.

## gpu_mem

A setting in `/boot/firmware/config.txt` that reserves memory for the Pi's video chip. rtspwall needs `gpu_mem=256` for four or more concurrent 1080p streams. `sudo rtspwall doctor --fix` sets it, after asking. A reboot is needed for it to take effect.

## H.264 and H.265

Two codecs. H.264 is the older and more common one. H.265 (also called HEVC) is newer and smaller. The Pi 4 decodes H.264 in hardware, and rtspwall supports only that. Many cameras can send either. Set yours to H.264 ([how](cameras.md#switching-a-camera-to-h264-or-a-sub-stream)).

## HDMI-A-1 and HDMI-A-2

The kernel's names for the Pi 4's two HDMI ports. Use them with `CONNECTOR`. Either port works, and by default rtspwall uses the first one with a display connected.

## HVS

Hardware Video Scaler. The part of the Pi's display hardware that shrinks and places picture layers while it sends the picture to the screen. It lets each camera sit on its own layer with no work for the CPU.

## Jitter buffer

A short waiting room for video frames. Network delivery is uneven, so frames wait briefly and are shown at an even pace. The wait is set by `BUFFER_MS`.

## Key frame

A video frame that is a complete picture. The frames after it only store what changed. A decoder cannot show a picture until it has received a key frame, so a new connection can wait a moment for one.

## Lite and Desktop

The two main Raspberry Pi OS images. Desktop has a graphical desktop. Lite has only a text console. rtspwall needs Lite, because a desktop holds the [DRM master](#drm-master).

## NVR

Network Video Recorder. A box or program that records your cameras, such as UniFi Protect, Frigate, or a Hikvision or Dahua recorder.

## Overlay plane

A layer of the display hardware. Each camera gets its own plane, and the hardware stacks the planes into the final picture. A Pi 4 has a limited number of planes. `--check-config` says how many your config needs.

## PLL

Phase-locked loop. A small correction loop that keeps frame timing even. rtspwall uses it so that recorders which stamp frames in uneven pairs do not make the picture hop.

## RTSP

Real Time Streaming Protocol. The way most IP cameras offer live video, with URLs like `rtsp://192.168.1.10:554/stream1`.

## RTSPS

RTSP inside an encrypted (TLS) connection, with URLs like `rtsps://192.168.1.10:7441/TOKEN`. UniFi Protect uses it. rtspwall keeps it by default. FFmpeg does not check the camera's certificate, so it hides the video from someone who only listens, not from someone who pretends to be the camera ([details](../SECURITY.md#the-unifi-tls-trade-off)).

## Sub-stream

A camera's second, smaller video stream, made for live viewing. Use it for a wall. It uses far less of the decoder than the full-size main stream and looks the same in a small tile.
