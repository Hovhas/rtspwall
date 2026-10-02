#!/usr/bin/env bash
# install-test.sh DEB [--pios]
#
# Installs DEB with apt in a clean Debian container (run as root) and checks
# the result. systemd is not running in the container, so this also proves the
# maintainer scripts cope without it. With --pios the Raspberry Pi OS archive
# is added as an apt source first, to catch dependency conflicts against Pi
# OS's own ffmpeg/libdrm packages.

set -euo pipefail

DEB=${1:?Usage: $0 DEB [--pios]}
PIOS=${2:-}
DEB=$(readlink -f "$DEB")
fail=0
ok() { printf 'ok    %s\n' "$*"; }
bad() { printf 'FAIL  %s\n' "$*" >&2; fail=1; }
check() { local d=$1; shift; if "$@"; then ok "$d"; else bad "$d"; fi; }

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends ca-certificates curl gnupg >/dev/null

if [[ $PIOS == --pios ]]; then
    # shellcheck source=/dev/null
    codename=$(. /etc/os-release && echo "$VERSION_CODENAME")
    curl -fsSL https://archive.raspberrypi.com/debian/raspberrypi.gpg.key \
        | gpg --dearmor -o /usr/share/keyrings/raspberrypi-archive-keyring.gpg
    echo "deb [signed-by=/usr/share/keyrings/raspberrypi-archive-keyring.gpg] http://archive.raspberrypi.com/debian/ $codename main" \
        >/etc/apt/sources.list.d/raspi.list
    apt-get update -qq
fi

cp "$DEB" /tmp/
apt-get install -y "/tmp/$(basename "$DEB")"

check "rtspwall --version" rtspwall --version
rtspwall --version

set +e
rtspwall --check-config /etc/rtspwall/cameras.conf >/tmp/cc.out 2>&1
rc=$?
set -e
cat /tmp/cc.out
if [[ $rc -eq 2 ]] && grep -qi 'no cameras configured' /tmp/cc.out; then
    ok "--check-config on the example config: exit 2, no cameras configured"
else
    bad "--check-config on the example config: exit $rc (want 2 + 'no cameras configured')"
fi

for u in rtspwall.service rtspwall-demo.service rtspwall-config.service rtspwall-config.path; do
    check "unit $u" test -f "/usr/lib/systemd/system/$u"
done
check "demo.conf" test -f /usr/share/rtspwall/demo/demo.conf
n=$(find /usr/share/rtspwall/demo -name '*.mp4' | wc -l)
check "5 demo clips (found $n)" test "$n" -eq 5
check "--check-config on the demo config" rtspwall --check-config /usr/share/rtspwall/demo/demo.conf

check "user rtspwall exists" getent passwd rtspwall
check "config is 0640 root:rtspwall" test "$(stat -c '%a %U:%G' /etc/rtspwall/cameras.conf)" = "640 root:rtspwall"

set +e
rtspwall doctor --no-hardware --no-probe >/tmp/doctor.out 2>&1
rc=$?
set -e
cat /tmp/doctor.out
check "doctor --no-hardware --no-probe ran (exit $rc)" test "$rc" -le 2

# Reinstall (upgrade path): the config must be left byte-identical.
echo '# local edit' >>/etc/rtspwall/cameras.conf
sum=$(sha256sum /etc/rtspwall/cameras.conf)
apt-get install -y --reinstall "/tmp/$(basename "$DEB")" >/dev/null
check "reinstall leaves the config untouched" test "$(sha256sum /etc/rtspwall/cameras.conf)" = "$sum"

apt-get purge -y rtspwall >/dev/null
check "purge removes /etc/rtspwall" test ! -e /etc/rtspwall

[[ $fail -eq 0 ]] || { echo "install-test: FAILED" >&2; exit 1; }
echo "install-test: all checks passed"
