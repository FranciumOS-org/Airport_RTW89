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
echo "--- kernel log (AirPort_RTW89 / rtw89), last minute"
/usr/bin/log show --last 1m --style compact \
    --predicate 'sender == "AirPort_RTW89" OR eventMessage CONTAINS "rtw89" OR eventMessage CONTAINS "rtw88:"' \
    | tail -30
if kmutil showloaded 2>/dev/null | grep -q com.rtw89.driver; then
    echo "still loaded" >&2
    exit 1
fi
echo "unloaded"
