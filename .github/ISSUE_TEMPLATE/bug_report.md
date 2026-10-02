---
name: Bug report
about: Something does not work or looks wrong
labels: bug
---

Please read [docs/troubleshooting.md](https://github.com/Hovhas/rtspwall/blob/main/docs/troubleshooting.md) first. **Remove tokens and passwords from everything you paste.**

## What happened

<!-- What did you expect, and what did you see instead? -->

## Environment

- Raspberry Pi model and RAM:
- OS (`cat /etc/os-release`, and 32/64-bit):
- `rtspwall --version`:
- Installed via (deb / tarball / source):
- Display and resolution:
- Camera / NVR brand and codec (must be H.264):
- `gpu_mem` in config.txt:

## `rtspwall --check-config` output

```
```

## 60 s statistics lines

```bash
journalctl -u rtspwall | grep ': 60s' | tail -n 12
```

```
```

## Kernel messages

```bash
dmesg | grep -iE 'vc4|bcm2835|codec'
```

```
```

## Other logs

<!-- journalctl -u rtspwall -b, around the time of the problem -->
