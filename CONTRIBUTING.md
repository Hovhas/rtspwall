# Contributing

Thanks for helping. Bug reports, hardware reports ("works on X") and pull requests are all welcome.

## Build and test

```bash
sudo apt install build-essential pkg-config libdrm-dev libavformat-dev libavcodec-dev libavutil-dev
make                  # builds src/rtspwall
make test             # unit tests; no Pi hardware or DRM/FFmpeg libraries needed
make test SANITIZE=1  # with AddressSanitizer and UBSan
```

`make test` builds only the pure logic (`src/pacing.c`, `src/layout.c`), so it runs on any development machine. ASan is off by default on Raspberry Pi OS kernels because it can fail to start there; it is on by default on macOS.

CI runs `make test` and ShellCheck on all shell scripts, then builds and tests on arm64 for Bookworm and Trixie. Run `shellcheck` locally if you touch a script.

## Code style

- C11. The build uses `-Wall -Wextra -Werror`; a warning fails the build.
- Keep logic that does not need Linux, DRM, V4L2 or FFmpeg in `pacing.c` and `layout.c`, and add tests for it (`src/test_pacing.c`, `src/test_layout.c`). Anything that can be tested without hardware should be.
- Match the surrounding code: tabs, short functions, comments that explain why.
- No new dependencies without discussion.

## Pull requests

- One change per pull request.
- Update the docs and `CHANGELOG.md` (under `Unreleased`) when behaviour or config changes.
- Use the pull request template.
- Test on a real Raspberry Pi 4 when the change touches decoding, DRM or timing, and paste the 60 s statistics lines into the pull request:

  ```bash
  journalctl -u rtspwall | grep ': 60s'
  ```

  Check the lines before pasting; they may contain camera names you do not want to publish.

## Reporting bugs and requesting features

Use the issue templates. For bugs we need the Pi model, OS, `rtspwall --version`, `--check-config` output, 60 s lines and the `dmesg` output listed in the template. Report security problems privately, see [SECURITY.md](SECURITY.md).

## License

By contributing you agree that your contribution is licensed under the [MIT License](LICENSE).
