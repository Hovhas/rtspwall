#!/usr/bin/env bash
# install.sh - install the prebuilt rtspwall from this tarball.
#
#   tar xzf rtspwall_*.tar.gz && cd rtspwall_*/ && sudo ./install.sh
#
# The binary is linked against the libraries of the Debian release named in the
# tarball, so use the tarball matching your OS (bookworm or trixie).
# Installs to /usr/local/bin (demo files to /usr/local/share/rtspwall/demo),
# creates the rtspwall system user, installs the example config only if none
# exists, installs the systemd units. Refuses to run if the .deb is installed.
# Does NOT enable or start the service and does not touch config.txt: run
# 'sudo rtspwall doctor --fix' to set gpu_mem (asks first, makes a backup).

set -euo pipefail

[[ $EUID -eq 0 ]] || { echo "Run as root: sudo $0" >&2; exit 1; }
cd "$(dirname "$0")"

CONF_DIR=/etc/rtspwall
PREFIX=/usr/local
UNIT_DIR=/etc/systemd/system
UNITS=(rtspwall.service rtspwall-demo.service rtspwall-config.service rtspwall-config.path)

if command -v dpkg >/dev/null && dpkg -s rtspwall >/dev/null 2>&1; then
    echo "The rtspwall .deb package is installed. Remove it first (apt remove rtspwall)" >&2
    echo "or upgrade through the package instead of using this tarball." >&2
    exit 1
fi

if command -v apt-get >/dev/null && [[ -s DEPENDS ]]; then
    # Strip version constraints; for alternatives (a | b) take the first.
    mapfile -t PKGS < <(sed 's/ *(.*)//; s/ *|.*//' DEPENDS)
    apt-get update -qq
    apt-get install -y --no-install-recommends "${PKGS[@]}"
fi

install -m 0755 rtspwall "$PREFIX/bin/rtspwall"
install -d -m 0755 "$PREFIX/share/rtspwall/demo"
install -m 0644 demo/* "$PREFIX/share/rtspwall/demo/"

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

for u in "${UNITS[@]}"; do
    sed -e "s|/usr/bin/rtspwall|$PREFIX/bin/rtspwall|g" \
        -e "s|/usr/share/rtspwall|$PREFIX/share/rtspwall|g" "$u" >"$UNIT_DIR/$u"
    chmod 0644 "$UNIT_DIR/$u"
done
systemctl daemon-reload
# Watch cameras.conf for edits. This does not start the wall itself.
systemctl enable --now rtspwall-config.path >/dev/null 2>&1 || true

echo "Installed. The service was NOT enabled or started. Next steps:"
echo "  sudo rtspwall demo          # 2x2 test wall, no cameras needed"
echo "  sudo rtspwall probe         # check a camera URL (codec, size, decoder budget)"
echo "  sudo rtspwall add NAME      # add a camera and start the wall"
echo "Check gpu_mem and the system: sudo rtspwall doctor   (--fix to repair)"
