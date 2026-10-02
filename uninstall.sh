#!/usr/bin/env bash
# uninstall.sh - remove a source install of rtspwall (see install.sh).
#
#   sudo ./uninstall.sh [--purge]
#
# Removes the binary and the systemd unit. The config in /etc/rtspwall is kept
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
    systemctl disable --now rtspwall.service 2>/dev/null || true
fi
rm -f /etc/systemd/system/rtspwall.service "$PREFIX/bin/rtspwall"
if command -v systemctl >/dev/null; then systemctl daemon-reload || true; fi
echo "Removed binary and unit."

if [[ $PURGE -eq 1 ]]; then
    rm -rf /etc/rtspwall
    if getent passwd rtspwall >/dev/null; then userdel rtspwall 2>/dev/null || true; fi
    if getent group rtspwall >/dev/null; then groupdel rtspwall 2>/dev/null || true; fi
    echo "Purged config and system user."
else
    echo "Kept /etc/rtspwall (use --purge to remove it and the system user)."
fi
