# Troubleshooting

Start with the logs:

```bash
journalctl -u rpi4-rtsp -b
dmesg | grep -iE 'vc4|bcm2835|codec'
sudo rpi4-rtsp --check-config /etc/rpi4-rtsp/cameras.conf
```

## Only the first few cameras start, `REQBUFS OUTPUT: Invalid argument`

Symptom: the fourth (or later) concurrent 1080p stream fails, and `dmesg` shows something like:

```
bcm2835_mmal_vchiq: vchiq_mmal_component_init: failed to create component -62 (Not enough GPU mem?)
bcm2835-codec: bcm2835_codec_create_component: failed to create component ril.video_decode
```

Cause: the VideoCore firmware heap (not Linux CMA) is exhausted at the default `gpu_mem`. Four or more concurrent 1080p decoders need `gpu_mem=256`.

Fix: add `gpu_mem=256` to `/boot/firmware/config.txt` (older systems: `/boot/config.txt`) and reboot. `install.sh` does this for you; the `.deb` only prints a warning. Reboot even if the failing state seems to clear: this failure can leave the decoder queue in a broken state until the next boot.

## Black screen, or the wrong screen

- Check which connectors exist. With a `CONNECTOR` that does not match, rpi4-rtsp logs every DRM card it scanned and the connectors on each, with their status.
- Set `CONNECTOR=HDMI-A-1` (or `HDMI-A-2`) in the config to pick the output.
- On a Pi 4 with two HDMI outputs there may be more than one DRM card. Set `DRM_DEVICE` (for example `/dev/dri/card1`) if the automatic choice is wrong.
- Make sure the display is connected and powered on at boot, then restart the service.

## `cannot become DRM master`

Only one client can be DRM master on a display. Something else holds it: an X server, a Wayland compositor, or another KMS program. rpi4-rtsp logs `is an X server or another compositor running?`.

Use Raspberry Pi OS **Lite**, or stop the desktop session (`sudo systemctl stop lightdm`, or the display manager your system uses) and disable it if you want the wall permanently. A plain text login on `tty1` is not a problem.

## A camera stays black or never connects

- Check the URL from another machine, but not while the wall is running against the same camera. Some NVRs share frames between sessions, so a second viewer steals frames from the wall. Stop the service first, or test with a different camera.
- The stream must be **H.264**. H.265/HEVC is not supported yet; switch the camera or channel to H.264, or use a restream that transcodes.
- Passwords with special characters may need percent-encoding in the URL.
- A `|` character cannot be used in a URL (it separates fields).
- RTSP must be enabled on the camera or NVR (for UniFi Protect: per camera channel).
- Set `FFMPEG_LOGLEVEL=warning` (or `info`) in the config and restart the service to see FFmpeg's own connection and RTP messages in the journal. URLs in those lines are masked too.

## Too many streams

The Pi 4's H.264 decoder is specified for about 1080p60 in total. Four 1080p15 streams already reach that. Examples that fit: 4 x 1080p15, 6 x 576p at 25-30 fps.

- Use each camera's sub-stream (lower resolution) for wall tiles.
- Remember that cameras in a rotation group all decode all the time.
- `--check-config` prints how many overlay planes and decoder instances your config needs.

Signs of overload: `dropped` above 1 % and falling `dec` values in the 60 s lines.

## Choppy video

Look at the 60 s line for the camera:

- `late` is high (more than a few per minute): raise `BUFFER_MS` (try 200), at the cost of extra latency.
- `dropped` is high while `late` is low: check `diag regulated ptsdelta`. A wide p5-p95 spread means the source delivers unevenly; a bigger `BUFFER_MS` helps.
- `dec` is below the camera's frame rate: the network or the camera is the bottleneck, or the decoder is overloaded (see above).
- One camera is consistently worse than the rest: give it a `delay_ms` in its config line.

## Verify reconnect behaviour

`scripts/reconnect-test.sh HOST PORT [SECONDS]` drops all outgoing TCP traffic to the source for SECONDS seconds (default 15) with a temporary nftables table, and removes it on any exit. Run it as root on the Pi and watch the wall: tiles should freeze or go black, then recover by themselves.

```bash
sudo ./scripts/reconnect-test.sh 192.168.1.5 554 15
```

Requires `nft` (`sudo apt install nftables`). In our test on a Pi 4 with six cameras, recovery took 4-6 s after the block was lifted.

## Reporting a problem

Open an issue using the bug report template. Include the Pi model, OS version, `rpi4-rtsp --version`, `--check-config` output, a few 60 s lines and the `dmesg` output above. Check your logs before pasting them: passwords, query strings and token-like path segments (for example UniFi Protect tokens) are masked, but masking is heuristic, so read what you paste.
