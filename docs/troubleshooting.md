# Troubleshooting

## Step 1: run doctor

Always start here. It checks the board, the decoder, the display, `gpu_mem`, desktop conflicts, file permissions and your config. Each problem comes with a fix to copy.

```bash
sudo rtspwall doctor
```

Exit status: 0 all fine, 1 warnings, 2 failures. Fix the first `FAIL` line, then run it again.

Then read the log of the service:

```bash
journalctl -u rtspwall -b
systemctl status rtspwall
```

`systemctl status` shows a status line such as `5/6 live; garage: login failed (401)`.

## Find your message

Look up the text you see in the log. The log shows the camera name first for camera messages, for example `garage: login failed (401): ...`. In the tables below, `NAME` stands for the camera name and `%s` or `N` for a value.

### Display and screen

| Log text | Cause | Fix |
|---|---|---|
| `cannot become DRM master on /dev/dri/cardN` followed by `... holds the display - a desktop session is running on this Pi` or `another program (a desktop, X server or compositor) holds the display` | Only one program can drive a screen ([DRM master](glossary.md#drm-master)). A desktop has it. | Boot to the console: `sudo raspi-config nonint do_boot_behaviour B1`, then `sudo reboot` as a separate command. The wall waits and starts by itself when the display is free (`waiting - the wall starts by itself as soon as the display is free (checking every 2 s)`). Best: use the Lite image. |
| `waiting for a display: switch the TV/monitor on or plug in HDMI (checking every 2 s)` | No display is connected or it is off. | Switch the TV on or plug in HDMI. The wall starts within seconds. |
| `found no connected display on /dev/dri/cardN` | Same, on that card. | Check the cable, the TV and the port. The log lists every connector and its status. |
| `CONNECTOR=HDMI-A-1: not connected (or no modes/usable CRTC yet)` | The TV is not on that port. | Set `CONNECTOR` to the port the TV uses (`HDMI-A-1` or `HDMI-A-2`), or remove the line. |
| `CONNECTOR=HDMI-A-2: no such connector on /dev/dri/cardN` | Typo or wrong card. | Pick a name from the `available connectors:` list below the message. |
| `no DRM device has connector ...` or `no usable DRM/KMS device found (set DRM_DEVICE to override); scanned:` | The card is missing or not ready. | Set `DRM_DEVICE=/dev/dri/cardN` for the card in the scan list. |
| `no /dev/dri/card* devices exist (is the vc4-kms-v3d overlay enabled?)` | The graphics driver is not loaded. | Make sure `dtoverlay=vc4-kms-v3d` is in `/boot/firmware/config.txt`, then reboot. |
| `DRM_DEVICE=...: No such file or directory` | The card name changed between kernels. | Remove `DRM_DEVICE` (auto) or use the right card. |
| `MODE=WxH@Hz: HDMI-A-1 does not offer that mode` | The TV lacks that mode. | Pick one from the `available modes on ...` list, or use `MODE=auto`. |
| `MODE=... not available on this display - using MODE=auto until it is` | Same, after the TV changed. | Nothing. It switches back when the mode returns. |
| `display mode: ... instead of the preferred ... (4K adds nothing ...)` or `(refresh under 50 Hz)` | Normal. `MODE=auto` chose 1080p or a faster mode. | To override, set `MODE=WxH@Hz`. |
| `display: HDMI-A-1 disconnected (TV off or in standby, input switched or cable out?) - the wall keeps running ...` | The TV went to standby, you switched input, or the cable came out. | Nothing. Wake the TV: the log says `connected again - re-reading its modes and setting the mode`. |
| `display: no page-flip event for 2 s on ... - recovering` or `display: N atomic commits failed in a row - re-probing ...` | The display stopped answering. | Usually self-heals. If it repeats, check the cable and report it. |
| `not enough overlay planes: the config has N cameras but CRTC N offers only N NV12-capable overlay plane(s)` | Every camera needs its own plane, including idle rotation members. | Remove cameras, or lower the count. |
| `display: setting the mode again failed: ... (will retry)` or `display: cannot lay out the wall on WxH: ... - keeping WxH` | The TV came back with a mode the wall could not use right away. | Usually self-heals. If it repeats, set `MODE=1920x1080@60` and report it. |
| `display: HDMI-A-1 offers no progressive mode - using its first mode` | The TV lists only interlaced modes. | Try another cable or port, or set `MODE` to a mode the log lists. |
| `driver lacks atomic/universal planes`, `CRTC/connector lacks atomic properties`, `found no primary plane for CRTC`, `plane N lacks ...`, `drmModeGetResources: ...`, `drmModeGetPlaneResources: ...`, `drmModeAtomicAlloc failed`, `CreatePropertyBlob: ...`, `initial atomic commit: ...`, `atomic commit: ...`, `CREATE_DUMB`, `MAP_DUMB`, `mmap dumb`, `AddFB2 primary` | The graphics driver is not the Pi's KMS driver, or the display refused a setting. | Check `dtoverlay=vc4-kms-v3d` (not `vc4-fkms-v3d`) and run `doctor`. Report with `doctor --report` if it repeats. |

### Decoder

| Log text | Cause | Fix |
|---|---|---|
| `no H.264 hardware decoder found at /dev/video10 (Raspberry Pi 5 has none)` | Pi 5, or a wrong `DECODER`. The service stops (exit 3) and does not restart. | On a Pi 5 see [the FAQ](faq.md#can-i-use-a-raspberry-pi-5). On a Pi 4 check `DECODER=` and that `/dev/video10` exists. |
| `decoder: cannot open /dev/video10: ... - the service user needs access to the video devices (group "video")` | The `rtspwall` user is not in the `video` group. | Reinstall the package, or `sudo adduser rtspwall video`, then restart. |
| `decoder: /dev/video10 does not exist yet - waiting up to 15 s` | The driver is still loading at boot. | Nothing. |
| `NAME: REQBUFS OUTPUT: Invalid argument (out of VideoCore memory? see gpu_mem in config.txt)` | The decoder memory is used up. `dmesg` shows `Not enough GPU mem`. | Run `sudo rtspwall doctor --fix`, then `sudo reboot` as a separate command. Reboot even if it seems to clear: the decoder can stay broken until the next boot. |
| `NAME: EXPBUF ...: ... - the decoder cannot export dmabuf` | Driver or kernel problem. | Run `doctor`, check `dmesg`, report with `doctor --report`. |
| `NAME: S_FMT`, `QBUF`, `DQBUF`, `STREAMON`, `QUERYBUF`, `mmap OUTPUT`, `G_FMT`, `REQBUFS CAPTURE`, `PrimeFDToHandle`, `AddFB2 ...`, `no free OUTPUT buffer for 10 s` | The decoder failed in the middle of setup or play. Often too much load. | Check the total with `sudo rtspwall probe /etc/rtspwall/cameras.conf`. Reboot. Report with `doctor --report` if it repeats. |

### Cameras

Camera lines have this shape: `NAME: <label>: <detail> - <hint>; next attempt in N s` (or `; reconnecting`). A line that repeats is shown once, then once every 5 minutes with `[repeated N times in the last 5 min]`.

| Label in the log | Cause | Fix |
|---|---|---|
| `login failed (401)` | Wrong user or password. | Check the account. Percent-encode special characters ([table](cameras.md#passwords-with-special-characters)). |
| `access denied (403)` | The account may not stream. | Allow RTSP or live view for it. |
| `stream not found (404)` | Wrong path, or an old UniFi token. | Copy the URL again from the camera or NVR. |
| `not H.264` (detail: `the stream is hevc, not H.264`) | The stream is H.265. | [Switch to H.264](cameras.md#switching-a-camera-to-h264-or-a-sub-stream) or use a sub-stream. |
| `resolution too large` | The decoder handles at most 1920x1920. | Use the sub-stream. |
| `decoder setup failed` | Out of decoder or GPU memory. | See the `gpu_mem` row above. |
| `connection refused` | RTSP is off or the port is wrong. | Enable RTSP. Check the port. |
| `timeout` or `unreachable` | Wrong address, or the network path is blocked. | Check the address, VLAN and firewall. |
| `stream ended` | The camera closed the stream. | Usually transient. It reconnects. |
| `stalled` (detail: `no data for 5 s`) | The stream stopped sending. | It reconnects by itself. If it repeats, check the network and the camera. |
| `NAME: connected, WxH` | Good. | Nothing. `(after N failed attempts)` says how long it took. |
| `NAME: pts ... - re-anchoring` | The camera's timestamps jumped. | Usually harmless. If constant, report the camera model. |
| `NAME: packet of N B larger than buffer ... - dropped` | A very large packet. | Lower the camera's bitrate or resolution. |
| `NAME: pacing: N packet(s) without a matching frame (the decoder skipped a corrupt frame)` | A damaged frame, often from packet loss. | Check the network. Use `FFMPEG_LOGLEVEL=warning`. |
| `NAME: unknown option ignored: ...` | Harmless. | Nothing. |
| `ffmpeg: ...` | FFmpeg's own message, with URLs masked. | Set `FFMPEG_LOGLEVEL=warning` or `info` for more. |
| `rotation: skipping NAME (no fresh frame for > 3 s), staying on NAME` | A camera in a rotation group has no picture. | Fix that camera. The wall carries on. |
| `NAME: CRITICAL: teardown gave up ...` or `CRITICAL: leak list full` | A buffer was kept on purpose instead of freed. | Report it with `doctor --report`. The counters `leaks_closed` and `leaks_active` show it in the 60 s lines. |

### Config and command line

| Message | Cause | Fix |
|---|---|---|
| `no cameras configured: every camera line is commented out or missing` | The config has no camera yet. The service stops (exit 2) and does not loop. | `sudo rtspwall add NAME`, or edit the file. |
| `line N: placeholder "CHANGE_ME" in the URL of camera NAME - replace it` | A template value is still there. | Put in the real value. |
| `line N: unknown key KEY (did you mean KEY?)` | A typo in a setting. | Fix the name. (The running service only warns: `config ...: warning: ...`.) |
| `config: fix the file and check it with: sudo rtspwall --check-config ...` | The config is invalid. | Run `--check-config` and fix the `line N:` message. |
| `field 3 is not an integer` or another `line N:` error | Often a `\|` in a password. | Write it as `%7C`. |
| `rtspwall: --mode is only valid with --check-config` and `--mode expects WxH` | Wrong command line. | Use `rtspwall --check-config --mode 1280x720 FILE`. |
| `rtspwall: all N cells of GRID=... are taken; give a CELL to share it (rotation group), or enlarge GRID` | `add` found no free cell. | `sudo rtspwall add NAME CELL`, or raise `GRID`. |
| `rtspwall: FILE has no GRID= line; add one first (e.g. GRID=2x2)` or `... uses manual tiles ... add only handles GRID layouts` | `add` needs a grid config. | Add `GRID=2x2`, or edit manual tiles by hand. |
| `rtspwall: a camera named "NAME" already exists` | Name taken. | Choose another name. |
| `rtspwall: not added (probe: FAIL)` | The probe failed. | Read the `fix:` line, or use `--force`. |
| `rtspwall: the demo starts and stops services: run it with sudo` | Missing `sudo`. | `sudo rtspwall demo`. |
| `rtspwall: no terminal for the hidden prompt; pipe the URL in instead` | No terminal available. | `rtspwall probe - < url.txt`. |
| `cameras.conf changed but is INVALID: rtspwall left running unchanged.` | You saved a bad file. | The old config keeps running. Fix and save again. |

## Symptoms

### Doctor says a desktop holds the display

`doctor` prints a line like `FAIL desktop  labwc (PID 123) running: a desktop holds the display (DRM master)`. This happens on the Desktop image. The fix it prints:

```bash
sudo raspi-config nonint do_boot_behaviour B1
```

Then reboot as a separate command:

```bash
sudo reboot
```

The Pi now boots to the text console, and the wall can take the screen. Flashing the **Lite** image avoids this ([why](faq.md#can-i-keep-the-desktop)).

### Video stalls or arrives late over Wi-Fi

`doctor` warns `Wi-Fi power save is on: causes stalls and late frames`. Turn it off, or use Ethernet:

```bash
sudo iw dev wlan0 set power_save off
```

This lasts until the next reboot. `doctor` prints a permanent `nmcli` command too.

### The screen is black

1. `sudo rtspwall doctor`.
2. Is the TV on the right input? Try the other HDMI port.
3. `journalctl -u rtspwall -b`: look for the display tables above.

### One tile is black

The camera is not connected. Read its lines in the journal (`journalctl -u rtspwall -b | grep NAME`) and use the camera table above. Or run `sudo rtspwall probe` with its URL.

### It worked, then I added cameras and it broke

You may have run out of decoder budget or `gpu_mem`. Run `sudo rtspwall probe /etc/rtspwall/cameras.conf` and read the total. Run `sudo rtspwall doctor`. See [Configuration](configuration.md#decoder-budget).

### The picture is choppy

Read the 60 s line of the camera (`journalctl -u rtspwall | grep ': 60s'`).

- `late` is high (more than a few per minute): raise `BUFFER_MS`, for example to `200`. It adds latency.
- `dropped` is high but `late` is low: the source delivers unevenly. A bigger `BUFFER_MS` helps. The `diag regulated ptsdelta` line shows how even the source is.
- `dec` is below the camera's frame rate: the network, the camera or the decoder is the bottleneck.
- One camera is worse than the rest: give it a `delay_ms` in its line.

### The picture is juddery on a 4K TV

Check the log line `display mode:`. With `MODE=auto` a 4K TV should run at 1920x1080. If not, set `MODE=1920x1080@60`.

### The TV was off and I switched it on

Nothing to do. The wall keeps running and sets the screen up again when the TV returns. This is built to recover on its own. If it does not, send us the log and the TV model.

### The edges are cut off

This is the TV's overscan. Set the TV's picture mode to "Just Scan", "Screen fit" or "1:1".

## Verify reconnect behaviour

`scripts/reconnect-test.sh HOST PORT [SECONDS]` blocks all outgoing TCP traffic to one camera or NVR for SECONDS seconds (default 15), with a temporary `nft` rule that it removes on exit. Run it as root on the Pi and watch the wall: tiles freeze or go black, then recover by themselves.

```bash
sudo ./scripts/reconnect-test.sh 192.168.1.5 554 15
```

It needs `nft` (`sudo apt install nftables`). The script is in the repository, not in the package.

## Reporting a problem

Run this and paste the output into the bug form. URLs and tokens are masked, but the masking is a heuristic, so **read what you paste**.

```bash
sudo rtspwall doctor --report
```

Questions go to [Discussions](https://github.com/Hovhas/rtspwall/discussions). Bugs go to the [issue forms](https://github.com/Hovhas/rtspwall/issues/new/choose).
