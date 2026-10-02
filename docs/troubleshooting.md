# Troubleshooting

## Step 1: run doctor

Always start here. It checks the board, the decoder, the display, `gpu_mem`, desktop conflicts, file permissions, UniFi plain-text settings and your config. Each problem comes with a fix to copy.

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
| `DRM_DEVICE=/dev/dri/cardN does not exist (yet) - waiting for it (checking every 2 s); if it never appears, check the path or use DRM_DEVICE=auto` | The card is not there yet (boot), or the name is wrong. The wall waits and logs `display: /dev/dri/cardN appeared` when it shows up. | If it never appears, remove `DRM_DEVICE` (auto) or use the right card. |
| `MODE=WxH@Hz: HDMI-A-1 does not offer that mode` | The TV lacks that mode. | Pick one from the `available modes on ...` list, or use `MODE=auto`. The wall does not stop: it runs with `auto` meanwhile. |
| `MODE=... not available on this display - using MODE=auto until it is` | Same, or the TV is in standby and offers only reserve modes. | Nothing. It checks every 30 s and switches when the mode returns. |
| `display: MODE=WxH is offered by HDMI-A-1 now - switching to it` | The mode you asked for is available now. | Nothing. The screen goes dark for a moment while it switches. |
| `layout: the manual tiles need a WxH screen but HDMI-A-1 offers WxH right now (TV in standby?) - waiting up to 60 s for a larger mode (checking every 2 s)` | Your manual tiles are larger than the screen mode the TV offers at the moment. | Switch the TV on. After 60 s the wall starts anyway. |
| `layout: WARNING: still WxH after 60 s - starting anyway; tiles reaching outside the screen are cut off (check the tiles with: rtspwall --check-config --mode WxH)` | The screen stayed too small. | Run the `--check-config --mode` command it prints, then fix the tiles or `MODE`. |
| `display mode: ... instead of the preferred ... (4K adds nothing ...)` or `(refresh under 50 Hz)` | Normal. `MODE=auto` chose 1080p or a faster mode. | To override, set `MODE=WxH@Hz`. |
| `display: HDMI-A-1 disconnected (TV off or in standby, input switched or cable out?) - the wall keeps running ...` | The TV went to standby, you switched input, or the cable came out. | Nothing. Wake the TV: the log says `connected again - re-reading its modes and setting the mode`. |
| `display: no page-flip event for 2 s on ... - recovering` | The display stopped answering. | Usually self-heals. If it repeats, check the cable and report it. |
| `display: N atomic commits failed in a row (ERROR) but HDMI-A-1 still accepts the background plane - the camera planes are rejected (scaler or memory bandwidth limit?); not setting the mode again` | The screen is fine, but the display hardware refuses the camera layers. Often too many or too large streams. | Check the total with `sudo rtspwall probe /etc/rtspwall/cameras.conf`. Lower the load. Report it if it repeats. |
| `display: N atomic commits failed in a row and even the background plane alone is rejected (ERROR) - re-probing HDMI-A-1 and setting the mode again` or `... and the modes of HDMI-A-1 changed - setting the mode again` | The display stopped accepting updates. | The wall resets the mode by itself. The screen goes dark for a moment. If it repeats, check the cable and report it. |
| `display: vblank wait refused N times in N ms (ERROR) - re-probing HDMI-A-1 and setting the mode again` | The display output seems to be off. | Same: it self-heals. If it repeats, check the cable and the TV. |
| `not enough overlay planes: the config has N cameras but CRTC N offers only N NV12-capable overlay plane(s)` | Every camera needs its own plane, including idle rotation members. | Remove cameras, or lower the count. |
| `display: setting the mode again failed: ... (will retry)` or `display: cannot lay out the wall on WxH: ... - keeping WxH` | The TV came back with a mode the wall could not use right away. | Usually self-heals. If it repeats, set `MODE=1920x1080@60` and report it. |
| `display: HDMI-A-1 offers no progressive mode - using its first mode` | The TV lists only interlaced modes. | Try another cable or port, or set `MODE` to a mode the log lists. |
| `display: not starting on DSI-1 - while the card has HDMI/DVI/DP connectors only those are picked automatically (set CONNECTOR=DSI-1 to use it)` | The TV is off or in standby, and another output (DSI panel, composite) reports connected. The wall waits for the TV instead. | Switch the TV on. To use that other output on purpose, set `CONNECTOR`. |
| `found no connected HDMI/DVI/DP display on /dev/dri/cardN` | No HDMI screen is on yet. | Switch the TV on. The wall starts by itself. |
| `display: HDMI-A-1 has been disconnected for 10 s but HDMI-A-2 is connected - moving the wall to HDMI-A-2` | You moved the cable to the other HDMI port, or a second screen is connected. | Nothing if you moved the cable. With two screens, set `CONNECTOR` to pin the wall. |
| `display: HDMI-A-1 (the connector the wall started on) has been connected for 10 s - moving the wall back to it` | The original screen is back. | Nothing. |
| `display: moving the wall to HDMI-A-2 failed: ... (will retry, backing off up to 60 s)` | The other output refused the wall. | Check that screen and cable, or set `CONNECTOR` to the output you use. Report it if it repeats. |
| `display: HDMI-A-2 is connected, but the wall's planes cannot be moved to its CRTC N - set CONNECTOR=HDMI-A-2 and restart to use it` | The wall cannot follow to that output while running. | Set `CONNECTOR` as the message says and restart: `sudo systemctl restart rtspwall`. |
| `driver lacks atomic/universal planes`, `CRTC/connector lacks atomic properties`, `found no primary plane for CRTC`, `plane N lacks ...`, `drmModeGetResources: ...`, `drmModeGetPlaneResources: ...`, `drmModeAtomicAlloc failed`, `CreatePropertyBlob: ...`, `initial atomic commit: ...`, `atomic commit: ...`, `CREATE_DUMB`, `MAP_DUMB`, `mmap dumb`, `AddFB2 primary` | The graphics driver is not the Pi's KMS driver, or the display refused a setting. | Check `dtoverlay=vc4-kms-v3d` (not `vc4-fkms-v3d`) and run `doctor`. Report with `doctor --report` if it repeats. |

### Decoder

| Log text | Cause | Fix |
|---|---|---|
| `no H.264 hardware decoder found at /dev/video10 (Raspberry Pi 5 has none)` | Pi 5, a wrong `DECODER`, or a device that is not an H.264 decoder (a second line then says what it is). `/dev/video10` was still missing after 15 s. The service stops (exit 3) and does not restart. | On a Pi 5 see [the FAQ](faq.md#can-i-use-a-raspberry-pi-5). On a Pi 4 check `DECODER=` and that `/dev/video10` exists. |
| `decoder: cannot open /dev/video10: ... - the service user needs access to the video devices (group "video")` | The `rtspwall` user is not in the `video` group, and it stayed that way after the 30 s wait below. The service stops (exit 3). | Reinstall the package, or `sudo adduser rtspwall video`, then `sudo systemctl restart rtspwall`. |
| `decoder: cannot open /dev/video10: ... - waiting up to 30 s (udev may still be setting its permissions at boot)` | At boot, udev has not yet given the device its permissions. | Nothing. The log says `decoder: /dev/video10 is accessible now` when it works. |
| `decoder: /dev/video10 does not exist yet - waiting up to 15 s` | The driver is still loading at boot. | Nothing. |
| `decoder: VIDIOC_QUERYCAP on /dev/video10 failed: ...` | The decoder did not answer when asked what it is. This may be temporary. The service restarts (exit 1). | Nothing at first. If it repeats, run `doctor`, check `dmesg` and report it. |
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
| `NAME: UniFi Protect URL: kept rtsps on port 7441 (TLS), Protect-only ?enableSrtp removed` | Normal. This is the default (`UNIFI_REWRITE=tls`). | Nothing. |
| `NAME: UniFi Protect rtsps URL rewritten to plain RTSP on port 7447 (UNIFI_REWRITE=plain) (...)` followed by `NAME: WARNING: plaintext: the UniFi token and video travel unencrypted on your LAN - UNIFI_REWRITE=tls keeps rtsps` | You set `UNIFI_REWRITE=plain`. | Remove that line from the config to go back to encrypted `rtsps`. See [SECURITY.md](../SECURITY.md#the-unifi-tls-trade-off). |
| `NAME: local file - played at its natural speed and looped` | The URL is a local video file, not a camera. | Nothing. It is used for tests and demos. |
| `NAME: unknown option ignored: ...` | Harmless. | Nothing. |
| `ffmpeg: ...` | FFmpeg's own message, with URLs masked. | Set `FFMPEG_LOGLEVEL=warning` or `info` for more. |
| `rotation: skipping NAME (no fresh frame for > 3 s), staying on NAME` | A camera in a rotation group has no picture. | Fix that camera. The wall carries on. |
| `NAME: CRITICAL: teardown gave up ...` or `CRITICAL: leak list full` | A buffer was kept on purpose instead of freed. | Report it with `doctor --report`. The counters `leaks_closed` and `leaks_active` show it in the 60 s lines. |
| `NAME: cleaned up N previously leaked buffer(s) after a confirmed flip` or `NAME: cleaned up N leaked buffer(s) on exit` | Buffers that were kept earlier are now freed. | Nothing. It is the all-clear after a `CRITICAL` line. |

### Other lines in the log

| Log text | Cause | Fix |
|---|---|---|
| `sd_notify: cannot send to $NOTIFY_SOCKET: ...` | The wall could not tell systemd its status. The `systemctl status` line may be missing or old. | Check that you start it with `systemctl`, not by hand. If it repeats, report it. |
| `out of memory` or `pthread_create NAME failed` | The Pi ran out of memory, or could not start a thread for that camera. | Remove cameras, close other programs, reboot. Report it if it repeats on a Pi with enough RAM. |
| `diag: busy_drops=N switches=N`, `NAME: diag regulated ptsdelta ...`, `diag: WARNING late1+=N ...` | Once-a-minute diagnostic lines. They are not errors. | Read what they mean in [the 60 second statistics line](configuration.md#the-60-second-statistics-line). |

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
| `rtspwall: too many arguments` | You gave more than one config path on the command line. | Give one `CONFIG` at most. |
| `rtspwall: not probing: this URL contains a user name/password` (or `a query string`, or `a token-like path segment`), then `Give the URL without the command line instead:` | `probe` refuses a URL with a secret as an argument. A command line is saved in shell history, shown by `ps` and logged by `sudo`. | `sudo rtspwall probe` (hidden prompt) or `sudo rtspwall probe - < url.txt`. `--insecure-argv` allows it anyway. Do not use it. |
| `rtspwall: FILE: not a regular file (probe takes a URL, a config file or a video file)` or `not a regular file (only regular files are probed)` | You gave a directory, a device or a pipe. | Give a URL, a config file or a video file. |
| `only rtsp://, rtsps:// and local files can be probed` | The URL uses another scheme such as `http://`. | Use the camera's RTSP URL. |
| `rtspwall: note: user 'rtspwall' does not exist (is rtspwall installed?); probing as nobody (uid N) instead of root` | The package is not installed, so the sandbox user is missing. As root, the probe still never runs as root. | Install the package (`get.sh`), or run `install.sh`. |
| `the probe process died (signal N, NAME) while reading the stream: a malformed stream, or a bug - please report it` | The probe child crashed on the stream data. | Report it with the camera model. Do not use that stream. |
| `could not drop root privileges to user rtspwall for the probe; not probing as root` | The sandbox could not switch to the unprivileged user. The probe is not done as root, by design. | Check that the user exists (`getent passwd rtspwall`). Report it if it does. |
| `rtspwall: another `rtspwall add` is editing this config; waiting for it to finish (lock: ...) ...` | Two `add` commands at the same time. The second waits. | Wait. If the first is stuck, stop it with Ctrl-C. |
| `rtspwall: FILE was changed by someone else while the camera was being probed; nothing was written - run add again` | The config changed during the probe (another editor, or a tool). | Run the same `add` command again. |
| `note: FILE is a symlink; editing its target PATH` | The config path is a symlink. | Nothing. `add` edits the real file. |
| `rtspwall: refusing to edit FILE: its directory DIR (MODE) is writable by other users, ...` | Other users could replace the config or the temporary file (which holds the password). | Run the `Fix:` command it prints (`sudo chown root:root DIR && sudo chmod go-w DIR`). |
| `rtspwall: FILE is not a regular file` | The config (or its lock file) is not a normal file. | Point `--config` at a normal file. |
| `cameras.conf changed but is INVALID (or unreadable by user rtspwall): rtspwall left unchanged. Run: sudo rtspwall --check-config /etc/rtspwall/cameras.conf` | You saved a bad file, or the user `rtspwall` cannot read it. | The old config keeps running. Fix the file or its permissions (`0640 root:rtspwall`) and save again. |
| `cameras.conf changed and is valid, but the demo is running: not touching it (run: sudo systemctl restart rtspwall.service when done)` | You saved the config while `rtspwall demo` was running. | Stop the demo, then run the command it prints. |
| `cameras.conf changed and is valid; rtspwall.service is stopped, leaving it stopped` | You stopped the wall on purpose. | `sudo systemctl start rtspwall` when you want it. |
| `cameras.conf changed and is valid: restarting failed rtspwall.service` | The wall had failed earlier. Your fix restarts it. | Nothing. |

### Lines that doctor prints

| Doctor line | Cause | Fix |
|---|---|---|
| `FAIL config  FILE (MODE) is writable by every user: only root may change it` (or `its group`, `its non-root owner`) | Someone other than root could change the config. That decides which URLs the service opens. | Run the fix line it prints: `sudo chown root:rtspwall FILE && sudo chmod 0640 FILE`. |
| `WARN unifi  NAME: UNIFI_REWRITE=plain plays it as plain RTSP (port 7447) - the access token and the video cross the network unencrypted` | `UNIFI_REWRITE=plain` for that camera. | Remove that setting (default `tls` keeps `rtsps`). |
| the same, with `... and the default route goes over Wi-Fi (wlan0): anyone in radio range ... can capture them` | Plain UniFi video over Wi-Fi. | Remove `UNIFI_REWRITE=plain`, or use Ethernet. |
| `check the gpu_mem lines under FILTER in config.txt by hand (doctor cannot tell whether that filter matches this Pi), or move gpu_mem to [all]` | A `gpu_mem` line sits under a `config.txt` filter doctor cannot evaluate. | Check it by hand. |
| `--fix: not changing FILE: it sets gpu_mem under FILTER, and doctor cannot tell whether that applies to this Pi. Set gpu_mem=N by hand (under [all])` | `--fix` will not guess. | Edit `config.txt` by hand, then `sudo reboot` as a separate command. |

After a successful `doctor --fix`, it prints `undo:     sudo cp BACKUP config.txt && sudo reboot`. Use it to go back.

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
