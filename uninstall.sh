#!/usr/bin/env bash
# uninstall.sh - remove a source install of rpi4-rtsp (see install.sh).
#
#   sudo ./uninstall.sh [--purge]
#
# Removes the binary and the systemd unit. The config in /etc/rpi4-rtsp is kept
# unless --purge is given (which also removes the system user).
# gpu_mem in config.txt is never changed back. Environment: PREFIX (default /usr/local).

set -euo pipefail

PREFIX=${PREFIX:-/usr/local}
PURGE=0

for arg in "$@"; do
    case $arg in
        --purge) PURGE=1 ;;
        -h | --help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; exit 2 ;;
    esac
done

[[ $EUID -eq 0 ]] || { echo "Run as root: sudo $0" >&2; exit 1; }

if command -v systemctl >/dev/null; then
    systemctl disable --now rpi4-rtsp.service 2>/dev/null || true
fi
rm -f /etc/systemd/system/rpi4-rtsp.service "$PREFIX/bin/rpi4-rtsp"
if command -v systemctl >/dev/null; then systemctl daemon-reload || true; fi
echo "Removed binary and unit."

if [[ $PURGE -eq 1 ]]; then
    rm -rf /etc/rpi4-rtsp
    if getent passwd rpi4-rtsp >/dev/null; then userdel rpi4-rtsp 2>/dev/null || true; fi
    if getent group rpi4-rtsp >/dev/null; then groupdel rpi4-rtsp 2>/dev/null || true; fi
    echo "Purged config and system user."
else
    echo "Kept /etc/rpi4-rtsp (use --purge to remove it and the system user)."
fi
