# Architecture

How rtspwall gets from an RTSP stream to a smooth picture without copying a frame. Read the [README](../README.md) first for the overview.

## Why a single DRM client

On the Pi 3, omxplayer decoded in the VideoCore and handed frames to DispmanX layers, which the hardware video scaler (HVS) composited during scanout. DispmanX is gone under KMS, and the display's hardware planes can be used by one DRM master at a time. Several independent players therefore cannot each get a plane of their own; going through X11 and XVideo costs a copy and a format conversion per frame and stream.

rtspwall is that one client. It becomes DRM master, gives each camera an overlay plane and lets the HVS scale and compose.

## Pipeline

```
RTSP (libavformat, demux only)
  -> V4L2 M2M H.264 decoder /dev/video10 (bcm2835-codec)
  -> dmabuf export from the decoder's capture buffer
  -> drmModeAddFB2
  -> atomic commit onto an overlay plane
```

The decoder outputs NV12 (linear). The frame is written by the decoder and read by the display; it is never copied. NV12 with the Broadcom SAND128 modifier would save some internal memory bandwidth, but brings an awkward stride convention, so the linear format is used.

Source files (in `src/`):

| File | Role |
|---|---|
| `main.c` | Command line, signals, start-up and shutdown order |
| `drm.c` | Find display, CRTC and planes; plane properties; primary framebuffer |
| `v4l2.c` | V4L2 M2M decoder: open, buffers, dmabuf export |
| `camera_thread.c` | Per camera: RTSP demux and watchdog, timestamps, decode, jitter buffer, teardown |
| `compositor.c` | vblank and flip events, frame selection, rotation, the 60 s statistics |
| `config.c`, `layout.c` | Config parsing, grid geometry, display-mode choice, UniFi URL rewrite, URL masking, `--check-config` |
| `pacing.c` | Pure logic: anchor, PLL, FIFO, rotation decisions, vblank counting, failure classes and backoff, log de-duplication |
| `cli.c`, `probe.c`, `add.c`, `demo.c`, `doctor.c` | The subcommands `probe`, `add`, `demo` and `doctor`, and the helpers they share |
| `budget.c`, `clilogic.c` | Pure logic: the decoder-budget model, probe hints, board detection |

`layout.c`, `pacing.c`, `budget.c` and `clilogic.c` have no Linux, DRM or FFmpeg dependencies, so `make test` exercises them on any machine. A gentler introduction is in [How it works](how-it-works.md).

## Smooth playback

A naive "show the latest frame" slot looks choppy when the network or the recorder delivers frames unevenly. Each camera instead has a jitter buffer, and the compositor picks frames against the display's actual vblank.

### Timestamps through the decoder

The stream's own timestamp (pts) is carried through the decoder, so each decoded frame knows when the source produced it, not only when it arrived locally.

### Anchor: sliding minimum

For each frame, `arrival - pts` includes the network delay. The anchor is the **sliding minimum** of that value over a 10 s window. It follows real changes in the path but does not lock onto a single extreme sample. A frame's target display time is roughly `pts + anchor + BUFFER_MS` (plus the camera's optional `delay_ms`).

Synthetic timestamps (created when no arrival time is queued) are never fed into the anchor, the PLL or the jitter histogram, so they cannot poison the window.

### PLL: pairs of frames

Some recorders stamp 30 fps streams in **pairs**: the pts delta alternates between about 7 ms and 60 ms instead of 33 ms and 33 ms. With raw pts, both frames of a pair become ripe at the same vblank, and frame selection drops the older one. On one NVR this gave up to 50 % dropped frames.

The PLL (`PACING_PLL_ALPHA_NUM/DEN`, alpha = 1/16, window of 32 frames) regulates the timeline toward the source's nominal frame period without needing to know which frames belong together. Target times are computed on the regulated timeline instead of raw pts. The `diag regulated ptsdelta` line in the log shows the result: a tight p5-p95 span around the nominal period.

### Why a buffer of 160 ms

`BUFFER_MS` is a trade-off between smoothness and latency. On the cameras we measured, 120 ms gave 0.7-0.9 % late frames at 30 fps (about 9 % at 24 fps), while 160 ms brought that down to 0-1 late frames per minute, for about 40 ms extra latency. Your sources may differ; see [troubleshooting](troubleshooting.md#the-picture-is-choppy).

### vblank-driven compositor

The compositor wakes on each vblank, confirms the previous flip, and selects the frame for each camera whose target time falls in the coming refresh. It does not sleep for a fixed interval. If a flip is confirmed one or more vblanks after the target, the global `diag: WARNING late1+=...` line reports it.

## Rotation

Cameras whose tile is **exactly** the same (same cell in grid mode, or identical width/height/x/y in manual mode) form a rotation group. A camera with a unique tile is a one-member group and never rotates.

All cameras decode all the time, shown or not. Only which plane of a group is **attached** changes. A switch detaches the outgoing camera's plane and attaches the incoming one in the **same atomic commit** as the rest of the picture, so there is no black frame and no flicker. Once per group and vblank the rotation logic decides whether to switch. If the next camera has had no fresh frame for more than 3 s it is skipped (`rotation: skipping <name> ...` in the log).

The cost of this design is that every camera in a group uses a decoder instance and an overlay plane at all times.

## Teardown order

When a stream is torn down (reconnect after the watchdog fires, or shutdown), the camera thread first asks the compositor to detach the plane, and waits for confirmation. Only then are the framebuffers removed (`drmModeRmFB`). The screen therefore never shows a frame from a buffer that no longer exists.

If confirmation does not arrive within the timeout, the buffer still in use is deliberately leaked rather than destroyed (logged as `CRITICAL: teardown gave up`), and cleaned up after a later confirmed flip or on exit. The `leaks_closed` and `leaks_active` counters make this visible; both should normally be 0.

On shutdown the main thread wakes waiting camera threads immediately, so a stop does not wait out the full teardown timeout.

## Robustness

- RTSP watchdog: 5 s without data leads to a reconnect.
- `stimeout` does not exist in FFmpeg 7.1 and is silently ignored if given, so it is not relied upon.
- Failure classes: a camera failure is classed (login failed, not found, not H.264, resolution too large, decoder setup, refused, timeout, stream ended, stalled, unreachable). Failures a retry cannot fix back off from 5 s up to 60 s. Transient ones retry within a few seconds so recovery after an NVR reboot stays fast. Repeated identical log lines collapse into one line with a counter.
- Display recovery: with no display the process waits and polls instead of exiting. When the connector disconnects (TV standby, input switch, cable out) the wall keeps running and sets the mode again on reconnect. Repeated failed commits trigger a re-probe and a new mode set.
- Service state: the unit is `Type=notify`. The daemon sends `READY=1` and a `STATUS=` line such as `5/6 live; garage: login failed (401)` through `$NOTIFY_SOCKET`, with no libsystemd dependency.
- Exit codes: `2` (config error, no cameras) and `3` (no H.264 decoder) are listed in `RestartPreventExitStatus=`, so those problems end the service with a readable status instead of a restart loop. Other failures restart with `Restart=on-failure`, a start limit of none, and a growing delay (`RestartSteps` and `RestartMaxDelaySec`, which systemd 254 and later honour).
- Config watch: `rtspwall-config.path` fires on a change to `cameras.conf`. `rtspwall-config.service` runs `--check-config` and restarts the wall only if the file is valid and the wall is enabled.
- Demo: `rtspwall-demo.service` plays local clips through the same code path. Only file inputs are paced by their timestamps and looped. `rtsp://` inputs never are.
- Recovery test: `scripts/reconnect-test.sh` blocks the source for a set time; see [troubleshooting](troubleshooting.md#verify-reconnect-behaviour).
