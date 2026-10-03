#!/usr/bin/env bash
# build-deb.sh [VERSION]
#
# Builds dist/rtspwall_VERSION_DIST_ARCH.deb from the current checkout.
# DIST comes from /etc/os-release (VERSION_CODENAME, e.g. bookworm or trixie),
# ARCH from dpkg. VERSION defaults to the latest git tag (without "v"), or 0.0.0.
# A pre-release VERSION such as 0.1.0-rc1 is kept in file names; the package's
# Debian version gets every "-" turned into "~": 0.1.0~rc1, and a dev build
# 0.1.0-rc3-dev2 becomes 0.1.0~rc3~dev2 (see DEB_VERSION below).
# Build dependencies: build-essential pkg-config libdrm-dev libavformat-dev
# libavcodec-dev libavutil-dev ffmpeg (ffmpeg generates the demo clips with
# `make demo-clips`; build-time only, not a runtime Depends) (+ dpkg-dev for
# accurate Depends).
#
# Design notes:
#  - /etc/rtspwall/cameras.conf is NOT a conffile. It holds credentials, and
#    dpkg conffile prompts/purge semantics are a poor fit. postinst installs the
#    example only when no config exists; postrm purge removes it.
#  - The unit goes to /usr/lib/systemd/system (correct on bookworm and trixie,
#    both usr-merged).

set -euo pipefail
umask 022

cd "$(dirname "$0")/.."
ROOT=$PWD

VERSION=${1:-}
if [[ -z $VERSION ]]; then
    VERSION=$(git describe --tags --abbrev=0 2>/dev/null | sed 's/^v//' || true)
    VERSION=${VERSION:-0.0.0}
fi
[[ $VERSION =~ ^[0-9][A-Za-z0-9.+~-]*$ ]] || { echo "Invalid version: $VERSION" >&2; exit 2; }
# VERSION is the release version as tagged (0.1.0-rc1): it goes into the file
# names and `rtspwall --version`. The Debian Version field gets EVERY "-"
# turned into "~" (0.1.0-rc1 -> 0.1.0~rc1, 0.1.0-rc3-dev2 -> 0.1.0~rc3~dev2).
# Why every one: a "-" left in the Debian version starts a Debian revision.
# "0.1.0-rc1" would be upstream 0.1.0 with revision rc1, i.e. newer than 0.1.0.
# "0.1.0~rc3-dev2" would be a non-native package (lintian then wants
# changelog.Debian.gz) that sorts AFTER 0.1.0~rc3, so apt refuses to install
# the release over the dev build. With no "-" the package is native, and each
# "~" sorts before the release it leads to: 0.1.0~rc2 < 0.1.0~rc3~dev2 <
# 0.1.0~rc3 < 0.1.0.
DEB_VERSION=${VERSION//-/'~'}

ARCH=$(dpkg --print-architecture)
# shellcheck source=/dev/null
DIST=$(. /etc/os-release && echo "${VERSION_CODENAME:-unknown}")
NAME=rtspwall
OUT=$ROOT/dist
STAGE=$OUT/staging
DEB=$OUT/${NAME}_${VERSION}_${DIST}_${ARCH}.deb

UNITS=(rtspwall.service rtspwall-demo.service rtspwall-config.service rtspwall-config.path)

command -v ffmpeg >/dev/null || {
    echo "ffmpeg is required to generate the demo clips (apt install ffmpeg)." >&2
    exit 1
}

rm -rf "$STAGE"
mkdir -p "$STAGE" "$OUT"

echo "== Building $NAME $VERSION ($DIST/$ARCH) =="
# Hardened build: PIE, stack protector (-strong + clash protection), _FORTIFY_SOURCE,
# full RELRO + BIND_NOW, CET/BTI branch protection where the arch has it.
# dpkg-buildflags exports CFLAGS/CPPFLAGS/LDFLAGS; the Makefile keeps
# its own warning flags (-Wall -Wextra -Werror) on top of them.
command -v dpkg-buildflags >/dev/null || { echo "dpkg-buildflags missing (apt install dpkg-dev)." >&2; exit 1; }
export DEB_BUILD_MAINT_OPTIONS="hardening=+all"
eval "$(dpkg-buildflags --export=sh)"
# Flags changed since any earlier plain build: always rebuild from scratch.
make clean
make VERSION="$VERSION"
make test
make demo-clips
make VERSION="$VERSION" DESTDIR="$STAGE" PREFIX=/usr install
strip --strip-unneeded "$STAGE/usr/bin/$NAME"
if command -v hardening-check >/dev/null; then
    # PIE, stack protector, fortify, relro, bindnow must all be "yes".
    hc=$(hardening-check "$STAGE/usr/bin/$NAME" || true)
    echo "$hc"
    if grep -E 'Position Independent|Stack protected|Fortify|Read-only relocations|Immediate binding' <<<"$hc" \
        | grep -qE 'no, |no$|unknown'; then
        echo "hardening-check: a protection is missing in $NAME (see above)." >&2
        exit 1
    fi
elif [ -n "${CI:-}" ]; then
    # CI must prove the hardening, not silently skip it.
    echo "hardening-check not installed in CI (apt install devscripts)." >&2
    exit 1
else
    echo "hardening-check not installed (apt install devscripts); skipping" >&2
fi

# --- Files ---
for u in "${UNITS[@]}"; do
    install -D -m 0644 "systemd/$u" "$STAGE/usr/lib/systemd/system/$u"
done
# `make install` must have put the demo config and clips in place.
[[ -f $STAGE/usr/share/rtspwall/demo/demo.conf ]] || {
    echo "demo files missing from $STAGE/usr/share/rtspwall/demo (make install)." >&2
    exit 1
}
# Config template for postinst; deliberately outside /usr/share/doc, which
# dpkg path-exclude rules (minimal images, some container setups) may drop.
install -D -m 0644 examples/cameras.conf "$STAGE/usr/share/rtspwall/cameras.conf.example"
DOC=$STAGE/usr/share/doc/$NAME
install -d -m 0755 "$DOC/examples"
install -m 0644 examples/* "$DOC/examples/"
install -m 0644 packaging/debian/copyright "$DOC/copyright"
if [[ -f README.md ]]; then install -m 0644 README.md "$DOC/README.md"; fi
{
    echo "$NAME ($DEB_VERSION) unstable; urgency=medium"
    echo
    echo "  * Release $VERSION."
    echo
    echo " -- rtspwall contributors <noreply@users.noreply.github.com>  $(date -R)"
} | gzip -9n >"$DOC/changelog.gz"
find "$DOC" -type f -name '*.md' -exec gzip -9nf {} +

install -d -m 0755 "$STAGE/DEBIAN"
for s in postinst prerm postrm; do
    install -m 0755 "packaging/debian/$s" "$STAGE/DEBIAN/$s"
done

# --- Depends ---
DEPENDS=""
if command -v dpkg-shlibdeps >/dev/null; then
    TMP=$(mktemp -d)
    mkdir -p "$TMP/debian"
    printf 'Source: %s\n\nPackage: %s\nArchitecture: any\n' "$NAME" "$NAME" >"$TMP/debian/control"
    DEPENDS=$(cd "$TMP" && dpkg-shlibdeps -O -e"$STAGE/usr/bin/$NAME" 2>/dev/null \
        | sed -n 's/^shlibs:Depends=//p')
    rm -rf "$TMP"
fi
if [[ -z $DEPENDS ]]; then
    echo "dpkg-shlibdeps unavailable or empty; falling back to ldd + dpkg -S" >&2
    DEPENDS=$(ldd "$STAGE/usr/bin/$NAME" | awk '/=> \//{print $3}' | while read -r lib; do
        dpkg -S "$(readlink -f "$lib")" 2>/dev/null | head -n1 | cut -d: -f1
    done | sort -u | paste -sd, - | sed 's/,/, /g')
fi
DEPENDS=${DEPENDS:+$DEPENDS, }adduser

# --- Control ---
SIZE=$(du -sk --exclude=DEBIAN "$STAGE" | cut -f1)
cat >"$STAGE/DEBIAN/control" <<CTRL
Package: $NAME
Version: $DEB_VERSION
Section: video
Priority: optional
Architecture: $ARCH
Maintainer: rtspwall contributors <noreply@users.noreply.github.com>
Installed-Size: $SIZE
Depends: $DEPENDS
Homepage: https://github.com/Hovhas/rtspwall
Description: RTSP video wall for the Raspberry Pi 4 (DRM/KMS, V4L2 hardware decode)
 Shows several RTSP camera streams on one HDMI display. Decodes H.264 with
 the Pi 4 V4L2 hardware decoder and puts every stream on its own hardware
 plane via DRM/KMS, without copying frames through the CPU or GPU.
 .
 Needs exclusive access to the display (no X11/Wayland on that output) and
 gpu_mem=256 in config.txt for four or more concurrent 1080p streams.
 The service is not enabled or started automatically; try
 "sudo rtspwall demo" first.
CTRL

dpkg-deb --build --root-owner-group "$STAGE" "$DEB"
echo "== Built $DEB =="
dpkg-deb -I "$DEB" | sed -n '1,/^ Description/p'

if command -v lintian >/dev/null; then
    lintian "$DEB" || echo "lintian reported issues (not fatal)" >&2
else
    echo "lintian not installed; skipping" >&2
fi
