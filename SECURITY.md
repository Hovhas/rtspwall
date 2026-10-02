# Security Policy

## Reporting a vulnerability

Please report vulnerabilities privately through GitHub Security Advisories: open the [Security tab](https://github.com/Hovhas/rtspwall/security/advisories/new) of the repository and choose "Report a vulnerability". Do not open a public issue for security problems.

Include what you found, how to reproduce it, and the affected version (`rtspwall --version`). This is a small project maintained in spare time; we will acknowledge reports as soon as we can but cannot promise a fixed response time.

## Supported versions

Only the latest release is supported.

## Credentials and logs

- `/etc/rtspwall/cameras.conf` contains camera credentials. The install scripts and the package create it as `0640 root:rtspwall` in a directory of mode `0750`. Keep it out of version control and out of backups you share.
- rtspwall masks the password in URLs of the form `rtsp://user:password@host/...` in its log and `--check-config` output. It **cannot** mask secrets that are part of the URL path or query (for example the token in a UniFi Protect URL). Review logs and config output before pasting them into an issue, a forum or a pull request.
- RTSP here is plain, unencrypted traffic on your network. Put cameras on an isolated network or VLAN where you can.

## Hardening

The systemd unit runs the service as an unprivileged system user with a restricted set of system calls and no capabilities. Additional options are documented (commented out) in `systemd/rtspwall.service`.
