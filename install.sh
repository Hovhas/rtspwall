#!/usr/bin/env bash
# install.sh - build and install rtspwall from source on Raspberry Pi OS.
#
#   git clone https://github.com/Hovhas/rtspwall && cd rtspwall
#   sudo ./install.sh
#
# Idempotent: safe to re-run (upgrades the binary and units, never overwrites
# an existing config). Does NOT enable or start the service, and never touches
# config.txt: run 'sudo rtspwall doctor --fix' to set gpu_mem (it asks first
# and makes a backup).
#
# Options:
#   --no-gpu-mem      accepted and ignored (kept for backward compatibility)
#   --no-build-deps   do not run apt-get (build dependencies already present)
#   -h, --help        show this help
#
# Environment: PREFIX (default /usr/local) selects the install prefix.

set -euo pipefail

PREFIX=${PREFIX:-/usr/local}
# PREFIX is sed-ed into the unit files and ends up in ExecStart=: allow only a
# plain absolute path (no spaces, quotes, "|", "&", "\\", "%" or "$").
[[ $PREFIX =~ ^/[A-Za-z0-9._/-]+$ ]] || { echo "Invalid PREFIX '$PREFIX': use an absolute path of letters, digits, . _ / - only." >&2; exit 2; }
SVC_USER=rtspwall
CONF_DIR=/etc/rtspwall
CONF=$CONF_DIR/cameras.conf
UNIT_DIR=/etc/systemd/system
UNITS=(rtspwall.service rtspwall-demo.service rtspwall-config.service rtspwall-config.path)
DO_DEPS=1

usage() { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; }

for arg in "$@"; do
    case $arg in
        --no-gpu-mem) ;; # no-op: config.txt is no longer modified here
        --no-build-deps) DO_DEPS=0 ;;
        -h | --help) usage; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; usage >&2; exit 2 ;;
    esac
done

[[ $EUID -eq 0 ]] || { echo "Run as root: sudo $0" >&2; exit 1; }

SRC_DIR=$(cd "$(dirname "$0")" && pwd)
cd "$SRC_DIR"
complete=1
[[ -f Makefile ]] || complete=0
for u in "${UNITS[@]}"; do [[ -f systemd/$u ]] || complete=0; done
[[ $complete -eq 1 ]] || {
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
        build-essential pkg-config libdrm-dev ffmpeg \
        libavformat-dev libavcodec-dev libavutil-dev
fi

echo "== Building and installing =="
make
make test
if command -v ffmpeg >/dev/null; then
    make demo-clips
else
    echo "  WARNING: ffmpeg not found; 'rtspwall demo' clips will be missing." >&2
fi
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

echo "== systemd units =="
# The unit files use packaged paths (/usr/bin, /usr/share); point them at $PREFIX.
for u in "${UNITS[@]}"; do
    sed -e "s|/usr/bin/rtspwall|$PREFIX/bin/rtspwall|g" \
        -e "s|/usr/share/rtspwall|$PREFIX/share/rtspwall|g" "systemd/$u" >"$UNIT_DIR/$u"
    chmod 0644 "$UNIT_DIR/$u"
    echo "  installed $UNIT_DIR/$u"
done
systemctl daemon-reload
# Watch cameras.conf for edits. This does not start the wall itself.
systemctl enable --now rtspwall-config.path >/dev/null 2>&1 || true

if [[ -x /usr/bin/rtspwall && $PREFIX != /usr ]]; then
    echo "  WARNING: /usr/bin/rtspwall exists too; check 'command -v rtspwall'." >&2
fi

echo
echo "Done. The service was NOT enabled or started. Next steps:"
echo "  sudo rtspwall demo          # 2x2 test wall, no cameras needed"
echo "  sudo rtspwall probe         # check a camera URL (codec, size, decoder budget)"
echo "  sudo rtspwall add NAME      # add a camera and start the wall"
echo "Config: $CONF   Logs: journalctl -u rtspwall -f"
echo "Check gpu_mem and the rest of the system: sudo rtspwall doctor   (--fix to repair)"
echo "Note: no other display server may run on the output used by rtspwall."
