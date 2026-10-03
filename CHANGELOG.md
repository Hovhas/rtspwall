# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project aims to follow [Semantic Versioning](https://semver.org/).

## [0.1.0] - Unreleased

First public release. Release candidates (`v0.1.0-rc1` and so on) are published from this section.

Formerly named rpi4-rtsp (renamed before the first release).

### Added

- RTSP to V4L2 M2M H.264 hardware decode to DRM/KMS overlay planes, one plane per camera, zero copy, no display server.
- Grid layout (`GRID=COLSxROWS` up to 8x8) and manual pixel layout; up to 16 cameras.
- Rotation groups: cameras sharing a tile take turns every `ROTATE_SECONDS`, switching in one atomic commit.
- Per-camera jitter buffer with sliding-minimum anchor and PLL; vblank-driven compositor.
- RTSP watchdog (5 s without data triggers a reconnect).
- New commands:
  - `rtspwall demo`: a 2x2 demo wall from bundled clips, no cameras needed.
  - `rtspwall probe [--insecure-argv] [URL | - | CONFIG | FILE]`: checks codec, size and frame rate against the decoder budget without playing the stream. The URL comes from a hidden prompt by default, or from stdin with `-`. Local video files can be probed. As root, each probe runs as the user `rtspwall` in a sandbox.
  - `rtspwall add [--config PATH] [--force] [--no-systemd] NAME [CELL]`: probes a camera, adds it to the config and starts the wall. It follows a symlinked config to its target, takes a lock (a second `add` waits), writes nothing if the config changed during the probe, removes its temporary file on Ctrl-C, and refuses to run if the config's directory is writable by other users. When `rtspwall-config.path` is active, it does the restart.
  - `rtspwall doctor [--config PATH] [--fix] [--yes] [--report] [--summary] [--no-hardware] [--no-probe]`: self-test with a fix for each problem, `--fix` for `gpu_mem` (asks first, makes a backup, prints the undo command) and `--report` with masked URLs for bug reports. It `FAIL`s if the config is writable by anyone but root, and `WARN`s for each camera with `UNIFI_REWRITE=plain` (more strongly when the default route is Wi-Fi). It understands `gpu_mem_1024` and `config.txt` section filters: a `gpu_mem` line under a filter it cannot evaluate gives a `WARN`, and `--fix` then changes nothing.
- `--check-config` (with `--mode WxH`), `--help`, `--version`.
- Config keys: `MODE` (`auto` or `WxH[@Hz]`; a 4K TV runs at 1080p60 under `auto`), `UNIFI_REWRITE` (`tls`, `plain` or `off`; `auto` is an alias for `tls`; UniFi Protect URLs can be pasted as shown), `DRM_DEVICE`, `FFMPEG_LOGLEVEL`.
- Exit codes with a meaning for systemd: `2` (config error or no cameras) and `3` (no H.264 hardware decoder). The unit does not restart on these.
- systemd units: `rtspwall.service` (`Type=notify`, status such as `5/6 live; garage: login failed (401)`), `rtspwall-demo.service`, and `rtspwall-config.path` with `rtspwall-config.service`, which restart the wall when a valid config is saved. A wall in the `failed` state is started again after a valid save, nothing happens while the demo runs, a wall you stopped on purpose stays stopped, and the check runs as the user `rtspwall`.
- Self-healing: waits for a display instead of exiting, recovers from TV standby, input changes and an unplugged cable, names the program that holds the screen, and backs off on camera failures that a retry cannot fix. It also waits, instead of exiting, for a `DRM_DEVICE` that does not exist yet, for a `MODE` the display does not offer (it runs with `auto` and switches to `MODE` when it is offered), for a screen big enough for manual tiles (up to 60 s, then it starts anyway) and for access to the decoder (up to 30 s). A failed `VIDIOC_QUERYCAP` is retried (exit 1). Exit 3 comes only when `/dev/video10` is missing after 15 s or is not an H.264 decoder (a Pi 5).
- Per-camera 60 s statistics line and diagnostic lines in the journal.
- `.deb` packages (Bookworm, Trixie, arm64), release tarball, `get.sh` installer, `install.sh` and `uninstall.sh` for source installs, hardened systemd unit.
- `SHA256SUMS` and build-provenance attestations for releases. They cover the packages, the tarball, `get.sh` and `SHA256SUMS` itself (`gh attestation verify get.sh --repo Hovhas/rtspwall`).
- `scripts/reconnect-test.sh` for testing reconnect behaviour.
- Documentation: getting started, cameras, configuration, troubleshooting, FAQ, migration guide, comparison, compatibility, benchmarks, how it works, glossary.
- Issue forms (bug report, camera report), Discussions link, code of conduct and a support policy.

### Changed

- Renamed from rpi4-rtsp to rtspwall: binary, package, unit, `/etc/rtspwall`, system user `rtspwall`, docs and CI.
- The example config ships with every camera line commented out. A leftover `CHANGE_ME` or `<placeholder>` is an error, and `--check-config` also rejects unknown keys and suggests the nearest one. The running service only warns about unknown keys.
- The unit uses `Restart=on-failure` with `RestartPreventExitStatus=2 3` and a growing restart delay, instead of restarting on every exit.
- The package installs the config as `0640 root:rtspwall` and warns about a source install that shadows it. It does not start the service and does not edit `config.txt`.
- Logs, `--check-config`, `probe`, `add` and `doctor --report` mask passwords, query strings and token-like path segments in URLs.
- `UNIFI_REWRITE` defaults to `tls`: a UniFi Protect `rtsps` URL on port 7441 is kept and only `?enableSrtp` is removed. `plain` rewrites to `rtsp://HOST:7447/TOKEN` with a warning (the token and the video are then unencrypted). `off` uses the URL as written.
- `probe` refuses a URL with a password, query string or token on the command line. Use the hidden prompt or `-`. `--insecure-argv` allows it.
- `get.sh` never changes `gpu_mem` and no longer runs `doctor`. `--yes` and `--no-gpu-mem` are accepted and do nothing. It ends with `After adding cameras: sudo rtspwall doctor`, and so does the message of the package. Run `doctor` after you add cameras.
- `install.sh` accepts only a `PREFIX` that matches `^/[A-Za-z0-9._/-]+$`.
- GitHub Actions upgraded to Node 24 versions: `actions/checkout` v7, `actions/upload-artifact` v7, `actions/download-artifact` v8, `actions/attest-build-provenance` v4.
- Build-provenance attestation is skipped for private repositories.
- The README has a rendered demo illustration (`docs/media/demo.gif`).
- The per-camera diagnostic line ends with a new field, `bufs=N`: the decoder buffers the camera holds (usually 16 while it plays). It must not grow from one reconnect to the next.
- New log line when packets arrive before the first picture of a connection: `NAME: pacing: first frame after N packet(s) (normal at connect)`.
- `doctor` lists kernel 6.18.50+rpt-rpi-v8 as tested.
- `doctor` and `add` say that 4 or more streams of 1080p or larger need `gpu_mem=256`, even when the frame rate is unknown. Before, only a load above half the decoder budget counted.

### Fixed

- The issue form `bug.yml` was invalid YAML and is corrected.
- `add` checked the decoder budget but not `gpu_mem`. Four 1080p streams at the default `gpu_mem=76` passed, and the decoder firmware ran out of memory and stayed locked up until a reboot. `add` now refuses when the new config needs more `gpu_mem` than is active (`rtspwall: not added (gpu_mem: ...)`), and tells you to run `sudo rtspwall doctor --fix` and then reboot, or to use the camera's sub-stream. `--force` adds anyway. If `vcgencmd` is missing, it only warns.
- Without `CONNECTOR`, the wall follows the HDMI cable to the other port after 10 s instead of waiting on a disconnected one, moves back when the original port returns, and never moves to (or starts on) DSI or composite outputs while an HDMI/DVI/DP output exists.
- `MODE=auto` no longer stays on a reserve mode (e.g. 1024x768) when the TV was in standby at boot; it switches once the real modes appear.
- DNS failures are reported as `unreachable` with a hint instead of a generic error.
- `probe` and the wall now pick the same video stream (first H.264 stream) when a source offers several.
- Local video files with an audio track no longer log codec errors.
- Retries after refused vblank waits back off up to 60 s instead of re-setting the mode every second.
- Reconnects while the display is off no longer wait 5 s for a plane that cannot detach; abandoned page flips can no longer lose decoder buffers.
- `rtspwall add` keeps the config file's extended attributes and ACLs.
- Reconnects no longer leak decoder (CMA) memory. The handles of the decoder buffers were not closed, so rc1 lost about 55 MB of free CMA memory per NVR outage with four 1024x576 cameras. Free CMA memory (`CmaFree` in `/proc/meminfo`) was the same after three outages with the fix.
- The line `NAME: pacing: N packet(s) without a matching frame (the decoder skipped a corrupt frame)` no longer appears when a connection starts. Packets that arrive before the first picture are not a damaged frame.
- A camera that reconnected at the exact moment one of its frames was being put on screen could make that display update fail. That frame is now dropped.
- Moving the wall to another HDMI port now detaches every camera from the old port first.
- A failed display update now wakes a camera that is waiting to shut down, so it does not wait out its timeout.

### Security

- Core dumps are off: `LimitCORE=0` in the units and `PR_SET_DUMPABLE` in the program.
- The binary is built hardened (PIE, stack protector, fortify, RELRO, bindnow). The package build checks it, and CI runs the build.
- `probe` and `add` never parse a stream as root: each probe runs as the user `rtspwall` in a sandbox, and it does not run if the privileges cannot be dropped.
- `doctor` fails on a config that others can write. `add` refuses a config directory that others can write. The watcher validates the config as the user `rtspwall`.
- URL masking also covers passwords with unencoded `/`, `?`, `#`, `"` or `'`, a user info part without a colon, and credentials in the path or in parameters (XMEye `user=...&password=...`, Foscam `;pwd=...`). It is still a heuristic: read what you paste.
- With `UNIFI_REWRITE=tls`, FFmpeg does not verify the camera's certificate. The encryption stops passive eavesdropping, not an active man-in-the-middle.

### Known limitations

- H.264 only; no H.265/HEVC.
- Raspberry Pi 4 only (Pi 400 and CM4 untested). Raspberry Pi 5 is not supported.
- Tested on hardware: Pi 4 (8 GB), Trixie, UniFi Protect. Bookworm and other camera brands are untested. The earlier 6-camera UniFi test used the plain rewrite. The `tls` default (`rtsps`) was later tested with 4 UniFi Protect cameras on the same Pi (Trixie, kernel 6.18.50+rpt-rpi-v8, build 0.1.0-rc2-dev): all four connected and played. A display at 1920x1080 at 60 Hz on HDMI-A-2 (HDMI1) worked, and `MODE=auto` picked the preferred mode. The dropped-frame, latency, recovery and CPU numbers of that run are in [docs/benchmarks.md](docs/benchmarks.md). No long-run test is documented.
- `gpu_mem=256` is required for 4 or more concurrent 1080p streams.

### Upgrade notes

- There is no earlier release to upgrade from. If you ran a development build under the old name `rpi4-rtsp`, remove it, then install rtspwall and copy your camera lines into `/etc/rtspwall/cameras.conf`.
- Later versions: run `get.sh` again, or `sudo apt install` the new package. Your config is kept, and a running wall is restarted.
