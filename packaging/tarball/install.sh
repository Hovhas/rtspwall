#!/usr/bin/env bash
# install.sh - install the prebuilt rpi4-rtsp from this tarball.
#
#   tar xzf rpi4-rtsp_*.tar.gz && cd rpi4-rtsp_*/ && sudo ./install.sh
#
# The binary is linked against the libraries of the Debian release named in the
# tarball, so use the tarball matching your OS (bookworm or trixie).
# Installs to /usr/local/bin, creates the rpi4-rtsp system user, installs the
# example config only if none exists, installs the systemd unit. Does NOT
# enable or start the service and does not touch config.txt (set gpu_mem=256
# in /boot/firmware/config.txt and reboot for 4+ concurrent 1080p streams).

set -euo pipefail

[[ $EUID -eq 0 ]] || { echo "Run as root: sudo $0" >&2; exit 1; }
cd "$(dirname "$0")"

CONF_DIR=/etc/rpi4-rtsp

if command -v apt-get >/dev/null && [[ -s DEPENDS ]]; then
    # Strip version constraints; for alternatives (a | b) take the first.
    mapfile -t PKGS < <(sed 's/ *(.*)//; s/ *|.*//' DEPENDS)
    apt-get update -qq
    apt-get install -y --no-install-recommends "${PKGS[@]}"
fi

install -m 0755 rpi4-rtsp /usr/local/bin/rpi4-rtsp

if ! getent passwd rpi4-rtsp >/dev/null; then
    useradd --system --user-group --no-create-home \
        --home-dir /nonexistent --shell /usr/sbin/nologin rpi4-rtsp
fi
usermod -aG video rpi4-rtsp
if getent group render >/dev/null; then usermod -aG render rpi4-rtsp; fi

install -d -m 0750 -o root -g rpi4-rtsp "$CONF_DIR"
if [[ -e $CONF_DIR/cameras.conf ]]; then
    chown root:rpi4-rtsp "$CONF_DIR/cameras.conf"
    chmod 0640 "$CONF_DIR/cameras.conf"
else
    install -m 0640 -o root -g rpi4-rtsp cameras.conf "$CONF_DIR/cameras.conf"
fi

sed 's|/usr/bin/rpi4-rtsp|/usr/local/bin/rpi4-rtsp|g' rpi4-rtsp.service \
    >/etc/systemd/system/rpi4-rtsp.service
chmod 0644 /etc/systemd/system/rpi4-rtsp.service
systemctl daemon-reload

echo "Installed. Next steps:"
echo "  1. sudo nano $CONF_DIR/cameras.conf"
echo "  2. sudo rpi4-rtsp --check-config $CONF_DIR/cameras.conf"
echo "  3. sudo systemctl enable --now rpi4-rtsp"
