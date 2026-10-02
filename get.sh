#!/bin/sh
# rtspwall installer for Raspberry Pi OS (64-bit, Bookworm or Trixie), Pi 4.
#
#   curl -fsSL https://github.com/Hovhas/rtspwall/releases/latest/download/get.sh | sudo bash
#
# Downloads the .deb for your Debian release, verifies its SHA-256 against
# SHA256SUMS from the same release, and installs it with apt. Running it again
# upgrades (the config in /etc/rtspwall is left alone). It never reboots.
#
# Options:  --yes, --no-gpu-mem   accepted and ignored (kept so older docs and
#           scripts keep working); this installer no longer touches gpu_mem.
#           Run `sudo rtspwall doctor` after adding cameras: it says if gpu_mem
#           must be raised.
# Environment:  RTSPWALL_VERSION=v0.1.0-rc1   install that release instead of the latest
#
# Provenance of the files (GitHub build attestation):
#   gh attestation verify rtspwall_<dist>_arm64.deb --repo Hovhas/rtspwall
#
# Test hooks (not for normal use): RTSPWALL_TEST_ROOT=DIR reads
# DIR/proc/device-tree/model, DIR/etc/os-release and DIR/var/lib/dpkg/lock-frontend
# instead of the real files; RTSPWALL_TEST_ARCH overrides the dpkg architecture;
# RTSPWALL_PRECHECK_ONLY=1 stops after the checks that run before apt is touched.
#
# All logic is in main(), called on the last line: a download that was cut off
# halfway contains no call and therefore does nothing.

set -eu

REPO=Hovhas/rtspwall
SUPPORT_URL="https://github.com/$REPO#will-it-work-for-me"

say() { printf 'rtspwall: %s\n' "$*"; }
die() { printf 'rtspwall: %s\n' "$*" >&2; exit 1; }

precheck() {
    root=${RTSPWALL_TEST_ROOT:-}

    arch=${RTSPWALL_TEST_ARCH:-$(dpkg --print-architecture 2>/dev/null || uname -m)}
    case $arch in
        arm64 | aarch64) ;;
        armhf | armel | arm* )
            die "this is a 32-bit system ($arch). rtspwall needs 64-bit Raspberry Pi OS - see $SUPPORT_URL" ;;
        *)
            die "unsupported CPU architecture ($arch): rtspwall is built for the Raspberry Pi 4 (arm64) - see $SUPPORT_URL" ;;
    esac

    model=""
    if [ -r "$root/proc/device-tree/model" ]; then
        model=$(tr -d '\0' <"$root/proc/device-tree/model" || true)
    fi
    case $model in
        *"Raspberry Pi 5"* | *"Compute Module 5"*)
            die "this is a $model: rtspwall needs the Pi 4's hardware H.264 decoder, which the Pi 5 does not have - see $SUPPORT_URL" ;;
        "") say "warning: could not read the board model; continuing, 'rtspwall doctor' will check the hardware." ;;
    esac

    id="" codename=""
    if [ -r "$root/etc/os-release" ]; then
        # shellcheck disable=SC1091,SC1090
        id=$(. "$root/etc/os-release" && printf '%s' "${ID:-}")
        # shellcheck disable=SC1091,SC1090
        codename=$(. "$root/etc/os-release" && printf '%s' "${VERSION_CODENAME:-}")
    fi
    case "$id:$codename" in
        debian:bookworm | debian:trixie | raspbian:bookworm | raspbian:trixie) ;;
        *)
            die "unsupported OS (${id:-unknown} ${codename:-unknown}): only Raspberry Pi OS / Debian Bookworm and Trixie are supported - see $SUPPORT_URL" ;;
    esac
}

wait_for_dpkg_lock() {
    lock=${RTSPWALL_TEST_ROOT:-}/var/lib/dpkg/lock-frontend
    command -v fuser >/dev/null 2>&1 || return 0   # apt-get -o DPkg::Lock::Timeout still waits
    [ -e "$lock" ] || return 0
    waited=0
    while fuser "$lock" >/dev/null 2>&1; do
        if [ "$waited" -ge 300 ]; then
            die "the package manager has been busy for 5 minutes (unattended upgrade or another apt?). Try again later."
        fi
        if [ "$waited" -eq 0 ]; then
            say "another package manager is running; waiting up to 5 minutes for it to finish..."
        fi
        sleep 5
        waited=$((waited + 5))
    done
}

fetch() { # fetch URL OUTFILE
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --retry 3 -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$2" "$1"
    else
        die "neither curl nor wget found: sudo apt install curl"
    fi
}

main() {
    for a in "$@"; do
        case $a in
            --yes | -y | --no-gpu-mem) ;; # accepted, no-op (compatibility)
            -h | --help) echo "Usage: sudo bash get.sh   (--yes/--no-gpu-mem are accepted and ignored; RTSPWALL_VERSION=v0.1.0-rc1 picks a release)"; return 0 ;;
            *) die "unknown option: $a (try --help)" ;;
        esac
    done

    precheck
    [ -z "${RTSPWALL_PRECHECK_ONLY:-}" ] || { say "checks passed ($codename)"; return 0; }

    [ "$(id -u)" -eq 0 ] || die "run as root: curl -fsSL https://github.com/$REPO/releases/latest/download/get.sh | sudo bash"

    tag=${RTSPWALL_VERSION:-}
    if [ -n "$tag" ]; then
        case $tag in
            v[0-9]*) ;;
            *) die "RTSPWALL_VERSION must look like v0.1.0 or v0.1.0-rc1" ;;
        esac
        base=https://github.com/$REPO/releases/download/$tag
    else
        base=https://github.com/$REPO/releases/latest/download
    fi

    file=rtspwall_${codename}_arm64.deb
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT INT TERM
    chmod 0755 "$tmp"   # apt's sandbox user must be able to read the .deb

    say "downloading $file${tag:+ ($tag)}..."
    fetch "$base/$file" "$tmp/$file"
    fetch "$base/SHA256SUMS" "$tmp/SHA256SUMS"

    line=$(awk -v f="$file" '$2 == f || $2 == "*" f { print $1 "  " f; exit }' "$tmp/SHA256SUMS")
    [ -n "$line" ] || die "$file is not listed in SHA256SUMS; not installing."
    (cd "$tmp" && printf '%s\n' "$line" | sha256sum -c - >/dev/null) \
        || die "checksum mismatch for $file; not installing."
    say "checksum OK"
    chmod 0644 "$tmp/$file"

    wait_for_dpkg_lock
    export DEBIAN_FRONTEND=noninteractive
    apt-get -o DPkg::Lock::Timeout=300 update
    apt-get -o DPkg::Lock::Timeout=300 install -y "$tmp/$file"

    cat <<'NEXT'

rtspwall is installed. It was not started and nothing was rebooted.
Next steps:
  sudo rtspwall demo          # 2x2 test wall, no cameras needed
  sudo rtspwall probe         # check a camera URL (codec, size, decoder budget)
  sudo rtspwall add NAME      # add a camera and start the wall
After adding cameras: sudo rtspwall doctor
Run this installer again at any time to upgrade; your config is kept.
NEXT
}

main "$@"
