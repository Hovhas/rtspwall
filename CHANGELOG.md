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
  - `rtspwall probe [URL | - | CONFIG]`: checks codec, size and frame rate against the decoder budget without playing the stream. The URL comes from a hidden prompt by default.
  - `rtspwall add [--config PATH] [--force] [--no-systemd] NAME [CELL]`: probes a camera, adds it to the config and starts the wall.
  - `rtspwall doctor [--config PATH] [--fix] [--yes] [--report] [--summary] [--no-hardware] [--no-probe]`: self-test with a fix for each problem, `--fix` for `gpu_mem` (asks first, makes a backup) and `--report` with masked URLs for bug reports.
- `--check-config` (with `--mode WxH`), `--help`, `--version`.
- Config keys: `MODE` (`auto` or `WxH[@Hz]`; a 4K TV runs at 1080p60 under `auto`), `UNIFI_REWRITE` (`auto` or `off`; UniFi Protect URLs can be pasted as shown), `DRM_DEVICE`, `FFMPEG_LOGLEVEL`.
- Exit codes with a meaning for systemd: `2` (config error or no cameras) and `3` (no H.264 hardware decoder). The unit does not restart on these.
- systemd units: `rtspwall.service` (`Type=notify`, status such as `5/6 live; garage: login failed (401)`), `rtspwall-demo.service`, and `rtspwall-config.path` with `rtspwall-config.service`, which restart the wall when a valid config is saved.
- Self-healing: waits for a display instead of exiting, recovers from TV standby, input changes and an unplugged cable, names the program that holds the screen, and backs off on camera failures that a retry cannot fix.
- Per-camera 60 s statistics line and diagnostic lines in the journal.
- `.deb` packages (Bookworm, Trixie, arm64), release tarball, `get.sh` installer, `install.sh` and `uninstall.sh` for source installs, hardened systemd unit.
- `SHA256SUMS` and build-provenance attestations for releases.
- `scripts/reconnect-test.sh` for testing reconnect behaviour.
- Documentation: getting started, cameras, configuration, troubleshooting, FAQ, migration guide, comparison, compatibility, benchmarks, how it works, glossary.
- Issue forms (bug report, camera report), Discussions link, code of conduct and a support policy.

### Changed

- Renamed from rpi4-rtsp to rtspwall: binary, package, unit, `/etc/rtspwall`, system user `rtspwall`, docs and CI.
- The example config ships with every camera line commented out. A leftover `CHANGE_ME` or `<placeholder>` is an error, and `--check-config` also rejects unknown keys and suggests the nearest one. The running service only warns about unknown keys.
- The unit uses `Restart=on-failure` with `RestartPreventExitStatus=2 3` and a growing restart delay, instead of restarting on every exit.
- The package installs the config as `0640 root:rtspwall` and warns about a source install that shadows it. It does not start the service and does not edit `config.txt`.
- Logs, `--check-config`, `probe`, `add` and `doctor --report` mask passwords, query strings and token-like path segments in URLs.

### Known limitations

- H.264 only; no H.265/HEVC.
- Raspberry Pi 4 only (Pi 400 and CM4 untested). Raspberry Pi 5 is not supported.
- Tested on hardware: Pi 4 (8 GB), Trixie, UniFi Protect. Bookworm and other camera brands are untested.
- `gpu_mem=256` is required for 4 or more concurrent 1080p streams.

### Upgrade notes

- There is no earlier release to upgrade from. If you ran a development build under the old name `rpi4-rtsp`, remove it, then install rtspwall and copy your camera lines into `/etc/rtspwall/cameras.conf`.
- Later versions: run `get.sh` again, or `sudo apt install` the new package. Your config is kept, and a running wall is restarted.
