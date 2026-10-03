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
- `sudo rtspwall probe` and `sudo rtspwall add NAME` read the URL from a **hidden prompt**, so it does not end up in your shell history, in `ps` output or in the `sudo` log. `probe` also reads the URL from stdin (`sudo rtspwall probe - < url.txt`).
- `probe` **refuses** a URL with a password, a query string or a token on the command line. `--insecure-argv` allows it anyway, and then the URL is in your history, in `ps` and in the `sudo` log.
- Only root may change `cameras.conf`. `sudo rtspwall doctor` reports a `FAIL` if the file is writable by anyone else. `add` refuses to edit a config whose directory other users can write to, and it removes its temporary file (which holds the password) if you press Ctrl-C.
- Core dumps are off for the wall (`LimitCORE=0` in the unit and `PR_SET_DUMPABLE` in the program), so the passwords in its memory do not end up in a dump file.

## What is masked in logs and reports

rtspwall masks credential-like parts of URLs in its log, in `--check-config`, in `probe` and `add` output, and in `rtspwall doctor --report`:

- the password in `rtsp://user:password@host/...`, including a password that contains an unencoded `/`, `?`, `#`, `"` or `'`
- a user info part without a colon (`rtsp://secret@host/...`), which may itself be a token
- the whole query string (`?channel=1&subtype=0` becomes `?***`)
- credentials in the path or in `;` parameters, for example the XMEye form `/user=admin&password=...` and the Foscam form `;pwd=...`
- path segments that look like tokens: 16 or more letters and digits, or 32 or more letters, digits, `_` and `-`. This covers the UniFi Protect token.

**The masking is a heuristic.** It leans towards hiding too much, but it can miss a secret in an unusual shape, and it does not touch camera names, IP addresses or host names. **Read what you paste** into an issue, a forum or a pull request.

## The UniFi TLS trade-off

UniFi Protect shows an encrypted URL, `rtsps://HOST:7441/TOKEN?enableSrtp`. The setting `UNIFI_REWRITE` decides what rtspwall does with it:

- **`tls` (default).** rtspwall keeps `rtsps` on port 7441 and only removes `?enableSrtp`. The video and the token are encrypted on the network. `auto`, the name used by older versions, means the same.
- **`plain`.** rtspwall rewrites the URL to `rtsp://HOST:7447/TOKEN`. The token and the video then cross your network **unencrypted**. rtspwall logs a warning, and `doctor` warns for each such camera (with a stronger message if the default route is Wi-Fi). Use it only if `tls` does not play, and only on an isolated, wired camera network (VLAN).
- **`off`.** The URL is used exactly as written.

**What `tls` does not give you.** FFmpeg, which rtspwall uses, does not verify the camera's certificate. The encryption protects against someone who only listens to your network (passive eavesdropping). It does **not** protect against someone who sits between the Pi and the camera and pretends to be the camera (active man-in-the-middle). Keep the cameras on a network you trust either way.

The project tested the `tls` path on hardware with 4 UniFi Protect cameras (see [Compatibility](docs/compatibility.md)). The earlier 6-camera test used the plain rewrite.

All other RTSP in a typical setup is also plain. Treat the camera network as a trusted segment.

## Hardening of the service

The unit `rtspwall.service` runs as the system user `rtspwall` (groups `video` and `render`) with:

- no capabilities, `NoNewPrivileges`, and a read-only file system outside of what it needs (`ProtectSystem=strict`, `ProtectHome`, `PrivateTmp`)
- protected kernel settings, modules, logs and control groups
- a restricted set of system calls (`@system-service` without `@privileged`) and of network address families
- `UMask=0077`
- no core dumps (`LimitCORE=0`), and the program also marks itself non-dumpable (`PR_SET_DUMPABLE`), so other processes of the same user cannot read its memory through `ptrace`

The binary is built hardened: position-independent code (PIE), stack protector, `_FORTIFY_SOURCE`, full RELRO and immediate binding (bindnow). The package build fails if one of these is missing, and CI runs that build.

`probe` and `add` never parse a camera stream as root. As root, each probe runs in a separate process as the unprivileged user `rtspwall` (or `nobody` if that user does not exist, with a note). If it cannot drop its privileges, it does not probe.

More options are documented, switched off, in `systemd/rtspwall.service`. They can break video, so enable them one at a time and check the picture.

## The probe sandbox

`sudo rtspwall probe`, `add` and `doctor` read data from cameras, and that data may be hostile. They never parse it as root. Each probe runs in its own child process, and the parent checks the result before it uses it. The child:

- drops root and runs as the system user `rtspwall` (or `nobody` if that user does not exist, with a note)
- keeps only that user's primary group, not `video` or `render`
- sets `no_new_privs` and turns off core dumps
- lets FFmpeg open only what it needs: protocols `rtsp`, `rtsps`, `tcp`, `udp`, `tls`, `rtp`, `srtp` and `crypto`; formats `rtsp`, `sdp` and `rtp`; codec `h264`

**If dropping privileges fails, the probe does not run.** The result says "not probing as root".

**Trade-off.** The child has the same rights as the service. A compromised probe could read `/etc/rtspwall/cameras.conf` (`0640`, `root:rtspwall`) and send signals to the service. That is the same damage as a compromised service, but it is never root.

The service uses the same FFmpeg whitelists.

**Supported sources.** Only `rtsp://`, `rtsps://` and local video files. Any other scheme (http, rtmp, udp, ...) is rejected by `--check-config` and at startup with the line number.

## Verifying a release

Each release publishes `SHA256SUMS` and build-provenance attestations next to the packages. Download the package and `SHA256SUMS` from the [Releases page](https://github.com/Hovhas/rtspwall/releases) into one folder, then check:

```bash
sha256sum -c --ignore-missing SHA256SUMS
```

To check that GitHub Actions built the file from this repository (needs the [GitHub CLI](https://cli.github.com/)):

```bash
gh attestation verify rtspwall_trixie_arm64.deb --repo Hovhas/rtspwall
```

Use `rtspwall_bookworm_arm64.deb` on Bookworm. The attestations also cover `get.sh` and `SHA256SUMS`:

```bash
gh attestation verify get.sh --repo Hovhas/rtspwall
```

If you do not want to pipe a script into a shell, use the "download, inspect, run" method from the [README](README.md#try-it-in-5-minutes).
