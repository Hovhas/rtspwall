#!/usr/bin/env bash
# install.sh - install the prebuilt rtspwall from this tarball.
#
#   tar xzf rtspwall_*.tar.gz && cd rtspwall_*/ && sudo ./install.sh
#
# The binary is linked against the libraries of the Debian release named in the
# tarball, so use the tarball matching your OS (bookworm or trixie).
# Installs to /usr/local/bin, creates the rtspwall system user, installs the
# example config only if none exists, installs the systemd unit. Does NOT
# enable or start the service and does not touch config.txt (set gpu_mem=256
# in /boot/firmware/config.txt and reboot for 4+ concurrent 1080p streams).

set -euo pipefail

[[ $EUID -eq 0 ]] || { echo "Run as root: sudo $0" >&2; exit 1; }
cd "$(dirname "$0")"

CONF_DIR=/etc/rtspwall

if command -v apt-get >/dev/null && [[ -s DEPENDS ]]; then
    # Strip version constraints; for alternatives (a | b) take the first.
    mapfile -t PKGS < <(sed 's/ *(.*)//; s/ *|.*//' DEPENDS)
    apt-get update -qq
    apt-get install -y --no-install-recommends "${PKGS[@]}"
fi

install -m 0755 rtspwall /usr/local/bin/rtspwall

if ! getent passwd rtspwall >/dev/null; then
    useradd --system --user-group --no-create-home \
        --home-dir /nonexistent --shell /usr/sbin/nologin rtspwall
fi
usermod -aG video rtspwall
if getent group render >/dev/null; then usermod -aG render rtspwall; fi

install -d -m 0750 -o root -g rtspwall "$CONF_DIR"
if [[ -e $CONF_DIR/cameras.conf ]]; then
    chown root:rtspwall "$CONF_DIR/cameras.conf"
    chmod 0640 "$CONF_DIR/cameras.conf"
else
    install -m 0640 -o root -g rtspwall cameras.conf "$CONF_DIR/cameras.conf"
fi

sed 's|/usr/bin/rtspwall|/usr/local/bin/rtspwall|g' rtspwall.service \
    >/etc/systemd/system/rtspwall.service
chmod 0644 /etc/systemd/system/rtspwall.service
systemctl daemon-reload

echo "Installed. Next steps:"
echo "  1. sudo nano $CONF_DIR/cameras.conf"
echo "  2. sudo rtspwall --check-config $CONF_DIR/cameras.conf"
echo "  3. sudo systemctl enable --now rtspwall"
