# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project aims to follow [Semantic Versioning](https://semver.org/).

## [0.1.0] - Unreleased

First public release.

Formerly named rpi4-rtsp (renamed before the first release).

### Added

- RTSP to V4L2 M2M H.264 hardware decode to DRM/KMS overlay planes, one plane per camera, zero copy, no display server.
- Grid layout (`GRID=COLSxROWS` up to 8x8) and manual pixel layout; up to 16 cameras.
- Rotation groups: cameras sharing a tile take turns every `ROTATE_SECONDS`, switching in one atomic commit.
- Per-camera jitter buffer with sliding-minimum anchor and PLL; vblank-driven compositor.
- RTSP watchdog (5 s without data triggers a reconnect).
- `--check-config` (with `--mode WxH`), `--help`, `--version`.
- `DRM_DEVICE` config key (default: auto).
- Per-camera 60 s statistics line and diagnostic lines in the journal.
- `.deb` packages (Bookworm, Trixie, arm64), release tarball, `install.sh`/`uninstall.sh` for source installs, hardened systemd unit.
- `scripts/reconnect-test.sh` for testing reconnect behaviour.

### Known limitations

- H.264 only; no H.265/HEVC.
- Raspberry Pi 4 only (Pi 400 and CM4 untested). Raspberry Pi 5 is not supported.
- `gpu_mem=256` is required for 4 or more concurrent 1080p streams.
