## What and why

<!-- One change per pull request. Link an issue if there is one. -->

## Checklist

- [ ] `make` and `make test` pass (C11, `-Werror`)
- [ ] New logic in the pure-logic files (`pacing.c`, `layout.c`, `budget.c`, `clilogic.c`) has tests
- [ ] Docs and `CHANGELOG.md` updated if behaviour or config changed
- [ ] New or changed user-visible log messages are in the table in `docs/troubleshooting.md` with the exact text
- [ ] Shell scripts pass `shellcheck` (if touched)
- [ ] No credentials or tokens in the diff, logs or screenshots

## Tested on hardware

<!-- For changes to decoding, DRM or timing: Pi model, OS, number of cameras, and the 60 s lines
     (journalctl -u rtspwall | grep ': 60s'). Write "not applicable" for docs or pure-logic changes. -->

```
```
