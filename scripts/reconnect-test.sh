#!/usr/bin/env bash
# reconnect-test.sh HOST PORT [SECONDS]
#
# Drops all outgoing packets to HOST:PORT (the RTSP source) for SECONDS
# seconds (default 15) with a temporary nftables table, then removes it.
# Watch the video wall: tiles should freeze or go black, then recover on their
# own once the block is lifted. The table is removed on any exit (Ctrl-C, error).
#
# Needs root and the nft(8) tool.

set -euo pipefail

TABLE=rpi4_rtsp_reconnect_test

usage() {
    echo "Usage: $0 HOST PORT [SECONDS]" >&2
    echo "  e.g. $0 192.0.2.10 554 15" >&2
    exit 2
}

[[ $# -ge 2 && $# -le 3 ]] || usage
HOST=$1
PORT=$2
SECS=${3:-15}

if ! [[ $PORT =~ ^[0-9]+$ ]] || ((PORT < 1 || PORT > 65535)); then
    echo "Invalid port: $PORT" >&2
    usage
fi
if ! [[ $SECS =~ ^[0-9]+$ ]] || ((SECS < 1)); then
    echo "Invalid seconds: $SECS" >&2
    usage
fi
[[ $EUID -eq 0 ]] || { echo "Run as root." >&2; exit 1; }
command -v nft >/dev/null || { echo "nft not found (apt install nftables)." >&2; exit 1; }

# Resolve to addresses (accepts IPs and hostnames), IPv4 and IPv6.
mapfile -t ADDRS < <(getent ahosts "$HOST" | awk '{print $1}' | sort -u)
((${#ADDRS[@]} > 0)) || { echo "Cannot resolve $HOST" >&2; exit 1; }

cleanup() {
    nft delete table inet "$TABLE" 2>/dev/null || true
    logger -t rpi4-rtsp-reconnect-test "DROP off" 2>/dev/null || true
    echo "Block removed."
}
trap cleanup EXIT
trap 'exit 130' INT TERM

nft delete table inet "$TABLE" 2>/dev/null || true
nft add table inet "$TABLE"
nft add chain inet "$TABLE" out "{ type filter hook output priority 0; }"
for a in "${ADDRS[@]}"; do
    if [[ $a == *:* ]]; then
        nft add rule inet "$TABLE" out ip6 daddr "$a" tcp dport "$PORT" drop
    else
        nft add rule inet "$TABLE" out ip daddr "$a" tcp dport "$PORT" drop
    fi
done
logger -t rpi4-rtsp-reconnect-test "DROP on: ${ADDRS[*]} port $PORT for ${SECS}s" 2>/dev/null || true
echo "Dropping traffic to ${ADDRS[*]} port $PORT for ${SECS}s ..."
sleep "$SECS"
