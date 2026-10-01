#!/bin/sh
# Stage the built kext as root:wheel and load it into the running kernel.
#
#   sudo tools/load.sh
#
# Only a person runs this (see CLAUDE.md): a bad load panics the machine.
# Nothing is installed: the staged copy lives under /private/var/tmp and the
# kext is gone after a reboot.
set -eu

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo tools/load.sh" >&2; exit 1; }

src="$(cd "$(dirname "$0")/.." && pwd)/build/out/AirPort_RTW89.kext"
stage=/private/var/tmp/AirPort_RTW89.stage
[ -d "$src" ] || { echo "no kext at $src; run: make kext" >&2; exit 1; }

if kmutil showloaded 2>/dev/null | grep -q com.rtw89.driver; then
    echo "com.rtw89.driver is already loaded; run sudo tools/unload.sh first" >&2
    exit 1
fi

rm -rf "$stage"
mkdir -p "$stage"
cp -R "$src" "$stage/"
chown -R root:wheel "$stage"
chmod -R go-w "$stage"

# Get everything on disk first, so a panic costs nothing but the reboot.
sync

echo "loading $stage/AirPort_RTW89.kext"
kmutil load -p "$stage/AirPort_RTW89.kext"

# start() runs the whole probe (firmware download included); give it time.
sleep 10
echo "--- kernel log (AirPort_RTW89 / rtw89), last 2 minutes"
log show --last 2m --style compact \
    --predicate 'sender == "AirPort_RTW89" OR eventMessage CONTAINS "rtw89" OR eventMessage CONTAINS "rtw88:"' \
    | tail -80
