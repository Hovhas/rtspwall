# Security Policy

## Reporting a vulnerability

Please report vulnerabilities **privately** through GitHub Security Advisories. Open the [Security tab](https://github.com/Hovhas/rtspwall/security/advisories/new) of the repository and choose "Report a vulnerability". Do not open a public issue or Discussion for a security problem.

Include what you found, how to reproduce it, and the affected version (`rtspwall --version`). This is a small project maintained in spare time. We aim to answer within 7 days, but we cannot promise a fix time.

## Supported versions

Only the latest release is supported.

## Threat model

rtspwall is meant for a Raspberry Pi on a home or small-office network that shows camera streams on a screen. It has no web interface and no network service of its own. It only makes outgoing RTSP connections to the cameras you list.

What we try to protect:

- **Camera credentials.** They are stored in one file and kept out of logs and bug reports as far as we can.
- **The Pi.** The service runs as an unprivileged user with a restricted system-call set, so a bug in stream parsing has less to work with.

What we assume and do not defend against:

- Someone with root on the Pi can read everything, including the config.
- Someone who can read your local network can see unencrypted RTSP video and credentials. See below.
- A malicious camera or NVR sends data that FFmpeg must parse. That is a real attack surface. Keep the Pi's packages up to date, and put cameras on an isolated network or VLAN where you can.

## Where credentials are stored

- `/etc/rtspwall/cameras.conf` holds camera URLs, usually with passwords. The package and `install.sh` create it as `0640`, owner `root`, group `rtspwall`, in a directory of mode `0750`. `chmod 600` would lock the service out.
- Keep the file out of version control and out of backups you share.
- Use a **read-only camera account** for the wall where the camera supports it.
- `sudo rtspwall probe` and `sudo rtspwall add NAME` read the URL from a **hidden prompt**, so it does not end up in your shell history, in `ps` output or in the `sudo` log. Passing a URL as an argument works, but then it does.

## What is masked in logs and reports

rtspwall masks credential-like parts of URLs in its log, in `--check-config`, in `probe` and `add` output, and in `rtspwall doctor --report`:

- the password in `rtsp://user:password@host/...`
- the whole query string (`?channel=1&subtype=0` becomes `?***`)
- path segments that look like tokens: 16 or more letters and digits, or 32 or more letters, digits, `_` and `-`. This covers the UniFi Protect token.

**The masking is a heuristic.** It leans towards hiding too much, but it can miss a secret in an unusual shape, and it does not touch camera names, IP addresses or host names. **Read what you paste** into an issue, a forum or a pull request.

## The UniFi TLS trade-off

UniFi Protect shows an encrypted URL, `rtsps://HOST:7441/TOKEN?enableSrtp`. By default (`UNIFI_REWRITE=auto`) rtspwall rewrites it to the plain form `rtsp://HOST:7447/TOKEN` and logs that it did. This makes the setup work when pasted as shown, but it means the video and the token cross your network **unencrypted**.

- If your cameras sit on an isolated VLAN or a trusted wired network, this is usually acceptable.
- To keep the URL exactly as written, set `UNIFI_REWRITE=off`. We have not verified that the encrypted form plays on every FFmpeg build, so it may not work.

All other RTSP in a typical setup is also plain. Treat the camera network as a trusted segment.

## Hardening of the service

The unit `rtspwall.service` runs as the system user `rtspwall` (groups `video` and `render`) with:

- no capabilities, `NoNewPrivileges`, and a read-only file system outside of what it needs (`ProtectSystem=strict`, `ProtectHome`, `PrivateTmp`)
- protected kernel settings, modules, logs and control groups
- a restricted set of system calls (`@system-service` without `@privileged`) and of network address families
- `UMask=0077`

More options are documented, switched off, in `systemd/rtspwall.service`. They can break video, so enable them one at a time and check the picture.

## Verifying a release

Each release publishes `SHA256SUMS` and build-provenance attestations next to the packages. Download the package and `SHA256SUMS` from the [Releases page](https://github.com/Hovhas/rtspwall/releases) into one folder, then check:

```bash
sha256sum -c --ignore-missing SHA256SUMS
```

To check that GitHub Actions built the file from this repository (needs the [GitHub CLI](https://cli.github.com/)):

```bash
gh attestation verify rtspwall_trixie_arm64.deb --repo Hovhas/rtspwall
```

Use `rtspwall_bookworm_arm64.deb` on Bookworm. If you do not want to pipe a script into a shell, use the "download, inspect, run" method from the [README](README.md#try-it-in-5-minutes).
