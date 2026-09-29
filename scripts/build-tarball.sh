#!/usr/bin/env bash
# build-tarball.sh VERSION
#
# Repackages the .deb built by build-deb.sh (in dist/) as a standalone
# rpi4-rtsp_VERSION_DIST_ARCH.tar.gz: binary, systemd unit, examples, license,
# the runtime package list (DEPENDS) and an install.sh for systems without dpkg
# packages. Run scripts/build-deb.sh VERSION first.

set -euo pipefail
umask 022

cd "$(dirname "$0")/.."
VERSION=${1:?Usage: $0 VERSION}
ARCH=$(dpkg --print-architecture)
# shellcheck source=/dev/null
DIST=$(. /etc/os-release && echo "${VERSION_CODENAME:-unknown}")
BASE=rpi4-rtsp_${VERSION}_${DIST}_${ARCH}
DEB=dist/$BASE.deb
[[ -f $DEB ]] || { echo "Missing $DEB - run scripts/build-deb.sh $VERSION first." >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
X=$WORK/x
D=$WORK/$BASE
mkdir -p "$X" "$D"
dpkg-deb -x "$DEB" "$X"

install -m 0755 "$X/usr/bin/rpi4-rtsp" "$D/rpi4-rtsp"
install -m 0644 "$X/usr/lib/systemd/system/rpi4-rtsp.service" "$D/rpi4-rtsp.service"
install -m 0644 "$X/usr/share/doc/rpi4-rtsp/examples/cameras.conf" "$D/cameras.conf"
if [[ -f LICENSE ]]; then install -m 0644 LICENSE "$D/LICENSE"; fi
dpkg-deb -f "$DEB" Depends | tr ',' '\n' | sed 's/^ *//; s/ *$//' >"$D/DEPENDS"
install -m 0755 packaging/tarball/install.sh "$D/install.sh"

tar -C "$WORK" --owner=0 --group=0 --numeric-owner -czf "dist/$BASE.tar.gz" "$BASE"
echo "Built dist/$BASE.tar.gz"
