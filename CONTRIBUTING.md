# Contributing

Thanks for helping. Bug reports, hardware reports ("works on X") and pull requests are all welcome. By taking part you agree to the [Code of Conduct](CODE_OF_CONDUCT.md).

## Where to go

| I want to... | Go to |
|---|---|
| Ask a question or show my wall | [Discussions](https://github.com/Hovhas/rtspwall/discussions) |
| Report a bug | The bug form in the [issue chooser](https://github.com/Hovhas/rtspwall/issues/new/choose). Paste `sudo rtspwall doctor --report` |
| Tell us a camera works or does not | The camera report form in the same chooser |
| Report a security problem | Privately, see [SECURITY.md](SECURITY.md) |
| Change code or docs | A pull request (below) |

Good first contributions: a vendor section in [docs/cameras.md](docs/cameras.md), a camera report, clearer wording for an error hint, or a fix in the docs.

## Support policy

- **Supported** means: a Raspberry Pi 4, Raspberry Pi OS Lite 64-bit (Bookworm or Trixie), and cameras that send H.264. Bugs in this setup get attention first.
- Other setups (Pi 5, H.265-only cameras, desktop images) are not supported. Reports about them are welcome as information, and may be closed with a pointer to the [FAQ](docs/faq.md).
- This is a spare-time project. Issues are triaged once a week, and we aim to reply within 7 days.

## Build and test

```bash
sudo apt install build-essential pkg-config libdrm-dev libavformat-dev libavcodec-dev libavutil-dev
make                  # builds src/rtspwall
make test             # unit tests; no Pi hardware or DRM/FFmpeg libraries needed
make test SANITIZE=1  # with AddressSanitizer and UBSan
```

`make test` builds only the pure logic (`pacing.c`, `layout.c`, `budget.c`, `clilogic.c`), so it runs on any development machine. ASan is off by default on Raspberry Pi OS kernels because it can fail to start there, and on by default on macOS. `make WERROR=0` builds without `-Werror`, for packagers.

CI runs `make test` and ShellCheck on all shell scripts, then builds, tests and installs the package on arm64 for Bookworm and Trixie. Run `shellcheck` locally if you touch a script.

## Developing without cameras

You do not need cameras, and you do not need to install the package to try a build.

1. Stop the installed wall, so it does not hold the screen: `sudo systemctl stop rtspwall`.
2. Build: `make`.
3. Run your build against a config file: `sudo ./src/rtspwall my-test.conf`. Check a file first with `./src/rtspwall --check-config my-test.conf`.

For streams, you have two choices.

- **Local clips.** The five demo clips in `demo/` are synthetic and CC0. A camera line can point at a file path instead of a URL. Files are played at their natural speed and looped. `demo/demo.conf` shows the format, with the installed paths. Copy it and change the paths to your `demo/` directory. `make demo-clips` rebuilds the clips with `ffmpeg`.
- **Real RTSP streams from a PC.** Run [mediamtx](https://github.com/bluenviron/mediamtx) and push a clip into it in a loop, for example:

  ```bash
  ffmpeg -re -stream_loop -1 -i demo/cam1.mp4 -c copy -f rtsp rtsp://127.0.0.1:8554/cam1
  ```

  Then use `rtsp://HOST:8554/cam1` in the config. This exercises the network, the watchdog and the reconnect paths. Stop and start mediamtx or the `ffmpeg` command to test a reconnect.

`sudo rtspwall demo` starts the **installed** demo service, not your build.

To install from source on a Pi, use `sudo ./install.sh` (options `--no-build-deps`). It builds, tests and installs the binary and the units, and does not start the service. Remove it with `sudo ./uninstall.sh [--purge]`. The package and a source install must not coexist: `install.sh` refuses to run when the package is installed.

## Code style

- C11. The build uses `-Wall -Wextra -Werror`. A warning fails the build.
- Keep logic that does not need Linux, DRM, V4L2 or FFmpeg in the pure-logic files, and add tests (`src/test_*.c`). Test everything you can without hardware.
- Match the surrounding code: tabs, short functions, comments that explain why.
- No new dependencies without discussion.

## Pull requests

- One change per pull request. Use the pull request template.
- Update the docs and `CHANGELOG.md` (under the top version) when behaviour or config changes.
- If you add or change a log message that users can see, update the table in [docs/troubleshooting.md](docs/troubleshooting.md) with the exact text.
- Follow [docs/STYLE.md](docs/STYLE.md) for docs.
- Test on a real Raspberry Pi 4 when the change touches decoding, DRM or timing, and paste the 60 second statistics lines (`journalctl -u rtspwall | grep ': 60s'`). Check them before pasting.

## Releasing

For maintainers.

- **Tag format:** `vX.Y.Z` or `vX.Y.Z-PRE`, where PRE is `rc1`, `beta2` and so on.
- **Tag and CHANGELOG:** the first `## [` heading in `CHANGELOG.md` must be the base version. The tags `v0.1.0-rc1`, `v0.1.0-rc2` and `v0.1.0` all release from `## [0.1.0]`. A final tag (without `-PRE`) also needs a date in the heading instead of `Unreleased`. The text of that section becomes the release notes, so it must not be empty. `scripts/release-notes.sh TAG` checks this.
- **Versions in names:** file names and `rtspwall --version` use `0.1.0-rc1`. The Debian package version is `0.1.0~rc1`, so that a release candidate sorts before the final release.
- **What `release.yml` does:** builds the packages for Bookworm and Trixie (arm64), installs them in a clean container as a test, and publishes the `.deb` and `.tar.gz` files, fixed-name copies (`rtspwall_bookworm_arm64.deb`, `rtspwall_trixie_arm64.deb`), `get.sh`, `SHA256SUMS` and build-provenance attestations. Tags with `-rc` or `-beta` become pre-releases automatically.
- **Pre-release testing:** `get.sh` takes `RTSPWALL_VERSION=v0.1.0-rc1` to install a specific tag.
- Keep [docs/compatibility.md](docs/compatibility.md) and [docs/benchmarks.md](docs/benchmarks.md) honest: add only what was run, with the date.

## License

By contributing you agree that your contribution is licensed under the [MIT License](LICENSE).
