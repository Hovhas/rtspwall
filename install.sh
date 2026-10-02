#!/usr/bin/env bash
# install.sh - build and install rtspwall from source on Raspberry Pi OS.
#
#   git clone https://github.com/Hovhas/rtspwall && cd rtspwall
#   sudo ./install.sh
#
# Idempotent: safe to re-run (upgrades the binary and unit, never overwrites
# an existing config). Does NOT enable or start the service.
#
# Options:
#   --no-gpu-mem      do not touch config.txt (gpu_mem)
#   --no-build-deps   do not run apt-get (build dependencies already present)
#   -h, --help        show this help
#
# Environment: PREFIX (default /usr/local) selects the install prefix.

set -euo pipefail

PREFIX=${PREFIX:-/usr/local}
SVC_USER=rtspwall
CONF_DIR=/etc/rtspwall
CONF=$CONF_DIR/cameras.conf
UNIT_DST=/etc/systemd/system/rtspwall.service
GPU_MEM_MIN=256
DO_GPU_MEM=1
DO_DEPS=1
NEED_REBOOT=0

usage() { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; }

for arg in "$@"; do
    case $arg in
        --no-gpu-mem) DO_GPU_MEM=0 ;;
        --no-build-deps) DO_DEPS=0 ;;
        -h | --help) usage; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; usage >&2; exit 2 ;;
    esac
done

[[ $EUID -eq 0 ]] || { echo "Run as root: sudo $0" >&2; exit 1; }

SRC_DIR=$(cd "$(dirname "$0")" && pwd)
cd "$SRC_DIR"
[[ -f Makefile && -f systemd/rtspwall.service ]] || {
    echo "Run this from a complete rtspwall checkout." >&2
    exit 1
}

if command -v dpkg >/dev/null && dpkg -s rtspwall >/dev/null 2>&1; then
    echo "The rtspwall .deb package is installed. Remove it first (apt remove rtspwall)" >&2
    echo "or upgrade through the package instead of installing from source." >&2
    exit 1
fi

echo "== Checking platform =="
MODEL=$(tr -d '\0' </proc/device-tree/model 2>/dev/null || true)
case $MODEL in
    *"Raspberry Pi 4"*) echo "  $MODEL" ;;
    *"Raspberry Pi 5"*)
        echo "  WARNING: $MODEL"
        echo "  WARNING: the Pi 5 has NO H.264 hardware decoder. rtspwall relies on the"
        echo "  WARNING: Pi 4 V4L2 decoder (/dev/video10) and will not work here." ;;
    "") echo "  WARNING: could not detect the board model; continuing (designed for Raspberry Pi 4)." ;;
    *) echo "  WARNING: '$MODEL' is not a Raspberry Pi 4; continuing anyway." ;;
esac
ARCH=$(dpkg --print-architecture 2>/dev/null || uname -m)
case $ARCH in
    arm64 | armhf | aarch64 | armv7l | armv8l) echo "  architecture: $ARCH" ;;
    *) echo "  WARNING: unexpected architecture '$ARCH' (expected arm64 or armhf)." ;;
esac

if [[ $DO_DEPS -eq 1 ]]; then
    echo "== Installing build dependencies =="
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y --no-install-recommends \
        build-essential pkg-config libdrm-dev \
        libavformat-dev libavcodec-dev libavutil-dev
fi

echo "== Building and installing =="
make
make test
make PREFIX="$PREFIX" install

echo "== System user and config =="
if ! getent passwd "$SVC_USER" >/dev/null; then
    useradd --system --user-group --no-create-home \
        --home-dir /nonexistent --shell /usr/sbin/nologin "$SVC_USER"
    echo "  created system user $SVC_USER"
fi
usermod -aG video "$SVC_USER"
if getent group render >/dev/null; then usermod -aG render "$SVC_USER"; fi

install -d -m 0750 -o root -g "$SVC_USER" "$CONF_DIR"
if [[ -e $CONF ]]; then
    echo "  keeping existing $CONF"
    chown root:"$SVC_USER" "$CONF"
    chmod 0640 "$CONF"
else
    install -m 0640 -o root -g "$SVC_USER" examples/cameras.conf "$CONF"
    echo "  installed example config: $CONF"
fi

echo "== systemd unit =="
# The unit file uses the packaged path /usr/bin; point it at $PREFIX/bin.
sed "s|/usr/bin/rtspwall|$PREFIX/bin/rtspwall|g" systemd/rtspwall.service >"$UNIT_DST"
chmod 0644 "$UNIT_DST"
systemctl daemon-reload
echo "  installed $UNIT_DST (ExecStart=$PREFIX/bin/rtspwall)"

if [[ $DO_GPU_MEM -eq 1 ]]; then
    echo "== Firmware GPU memory =="
    CONFTXT=/boot/firmware/config.txt
    [[ -f $CONFTXT ]] || CONFTXT=/boot/config.txt
    if [[ ! -f $CONFTXT ]]; then
        echo "  no config.txt found; set gpu_mem=$GPU_MEM_MIN yourself (>=4 concurrent 1080p decoders)."
    else
        CUR=$(sed -n 's/^gpu_mem=\([0-9]\+\)[[:space:]]*$/\1/p' "$CONFTXT" | tail -n1)
        if [[ -n $CUR && $CUR -ge $GPU_MEM_MIN ]]; then
            echo "  gpu_mem=$CUR in $CONFTXT (ok)"
        else
            BACKUP="$CONFTXT.rtspwall.bak"
            [[ -e $BACKUP ]] || cp -p "$CONFTXT" "$BACKUP"
            if [[ -n $CUR ]]; then
                sed -i "s/^gpu_mem=[0-9]\\+[[:space:]]*\$/gpu_mem=$GPU_MEM_MIN/" "$CONFTXT"
            else
                printf '\n# rtspwall: firmware heap for several concurrent H.264 decoders\n[all]\ngpu_mem=%s\n' \
                    "$GPU_MEM_MIN" >>"$CONFTXT"
            fi
            echo "  set gpu_mem=$GPU_MEM_MIN in $CONFTXT (backup: $BACKUP)"
            NEED_REBOOT=1
        fi
    fi
fi

echo
echo "Done. The service was NOT enabled or started. Next steps:"
if [[ $NEED_REBOOT -eq 1 ]]; then
    echo "  0. REBOOT: gpu_mem was changed and only takes effect after a restart."
fi
echo "  1. Edit the config:        sudo nano $CONF"
echo "  2. Validate it:            sudo rtspwall --check-config $CONF"
echo "  3. Start the video wall:   sudo systemctl enable --now rtspwall"
echo "     Logs:                   journalctl -u rtspwall -f"
echo "Note: no other display server may run on the output used by rtspwall."
