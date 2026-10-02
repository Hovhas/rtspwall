# How it works

For the curious. No code needed. If you want the details, read [Architecture](architecture.md) afterwards.

## The idea in one picture

A camera sends a compressed video stream. Someone has to unpack it and put it on the screen. rtspwall asks the Pi's own video chips to do the heavy work, so the CPU can mostly sit idle.

```mermaid
flowchart LR
    C[Camera or NVR] -->|RTSP over the network| D[rtspwall: reads the stream]
    D -->|compressed H.264| H[Pi 4 video decoder chip]
    H -->|finished picture in memory| S[Display hardware]
    S -->|HDMI| T[TV]
```

The same thing as plain text:

```
camera --network--> rtspwall --H.264--> decoder chip --picture--> display hardware --HDMI--> TV
                    (CPU: only reads              (does the         (shrinks and places
                     and schedules)               hard work)         each camera tile)
```

The important part is the arrow from the decoder to the display. The picture is never copied. The decoder writes it into memory and the display reads it from the same place. Copying a video picture is slow, and doing it for six cameras all day is wasteful.

## Why there is no desktop

A screen can be driven by only one program at a time. That program is called the [DRM master](glossary.md#drm-master). A desktop takes the job for itself, so a desktop and rtspwall cannot share a screen. This is why rtspwall asks for Raspberry Pi OS **Lite**, and why `doctor` tells you how to switch a desktop off.

Every camera gets its own display layer, an [overlay plane](glossary.md#overlay-plane). The display hardware (the [HVS](glossary.md#hvs)) shrinks each camera to its tile and puts the tiles together while it sends the picture to the TV. That is why six cameras do not load the CPU much.

## The decoder budget and `gpu_mem`

The decoder chip has a limit: about one 1080p picture stream at 60 frames per second in total, spread over all cameras. Many small streams fit. A few big ones do not. This is why we suggest the camera's [sub-stream](glossary.md#sub-stream). `rtspwall probe` adds up your cameras and tells you if they fit.

The decoder also needs memory that the Pi reserves for its video chip. That setting is [`gpu_mem`](glossary.md#gpu_mem). With four or more 1080p streams, 256 MB is needed. `rtspwall doctor` checks it and can set it for you.

## Why the picture stays smooth

Networks are not even. Frames arrive early, late or two at once. If the wall simply showed each frame as it arrived, the picture would stutter.

Instead each camera has a small waiting room, the [jitter buffer](glossary.md#jitter-buffer). Frames wait there for a moment, and the wall shows them at the right time on the display's own clock. The wait is `BUFFER_MS`, 160 ms by default. That is the price of smoothness: a little delay.

Some recorders stamp frames in pairs, which makes video look like it is hopping. A small correction loop, a [PLL](glossary.md#pll), evens that out.

## Rotation without a black frame

When several cameras share a tile, all of them keep running in the background. The wall only changes which one is shown, in the same screen update as everything else. So there is no black flash and no waiting for a reconnect.

## When something goes wrong

If a camera stops sending, the wall notices after 5 seconds and reconnects, with a growing pause for problems that a retry cannot fix (a wrong password, for example). If the TV goes to standby, the wall keeps running and sets the screen up again when the TV comes back.

## Why a Pi 4 and not a Pi 5

The Pi 4 has a chip that decodes H.264. The Pi 5 does not: it decodes H.265 in hardware but not H.264. rtspwall depends on the H.264 chip, so it needs a Pi 4. See the [FAQ](faq.md#can-i-use-a-raspberry-pi-5).
