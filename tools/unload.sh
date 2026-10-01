#!/bin/sh
# Unload the kext loaded by tools/load.sh.
#
#   sudo tools/unload.sh
#
# Only a person runs this (see CLAUDE.md).
set -eu

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo tools/unload.sh" >&2; exit 1; }

sync
kmutil unload -b com.rtw89.driver
sleep 2
# On the first unload the kext's "stopping" line never showed up in the unified
# log, so read the kernel message buffer, which gets every IOLog line directly.
echo "--- kernel message buffer (AirPort_RTW89 / rtw89)"
/sbin/dmesg | grep -E 'AirPort_RTW89|rtw89|rtw88' | tail -25
if kmutil showloaded 2>/dev/null | grep -q com.rtw89.driver; then
    echo "still loaded" >&2
    exit 1
fi
echo "unloaded"
