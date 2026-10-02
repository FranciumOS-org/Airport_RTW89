#!/bin/sh
# Save this boot's Wi-Fi logs (the driver's lines are matched by text too: loaded
# from the EFI, its sender is the kernel collection, not AirPort_RTW89) where Claude can read them:
#   build/log/user-driver.log   the driver's own lines
#   build/log/user-airportd.log macOS's Wi-Fi daemon, join-related lines only
#
#   sudo tools/collect_logs.sh
#
# Neither contains passwords: the driver never logs key material, and the
# daemon's lines are filtered to joins, scans' security fields and failures.
set -eu
[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo tools/collect_logs.sh" >&2; exit 1; }
here="$(cd "$(dirname "$0")/.." && pwd)"
out="$here/build/log"
mkdir -p "$out"
# From the boot on. Not --last boot: after a wall clock adjustment that misses
# everything since (log show warns about it), while --start does not.
boot="$(sysctl -n kern.boottime | sed -n 's/.*sec = \([0-9]*\).*/\1/p')"
start="$(date -r "$boot" '+%Y-%m-%d %H:%M:%S')"
/usr/bin/log show --start "$start" --style compact \
    --predicate 'process == "kernel" AND (sender == "AirPort_RTW89" OR eventMessage CONTAINS "AirPort_RTW89" OR eventMessage CONTAINS "[rtw89")' \
    > "$out/user-driver.log" 2>/dev/null
/usr/bin/log show --start "$start" --style compact \
    --predicate 'process == "airportd" AND (eventMessage CONTAINS[c] "join" OR eventMessage CONTAINS[c] "SAE" OR eventMessage CONTAINS[c] "WPA3" OR eventMessage CONTAINS[c] "password" OR eventMessage CONTAINS[c] "credential" OR eventMessage CONTAINS[c] "assoc" OR eventMessage CONTAINS[c] "fail")' \
    > "$out/user-airportd.log" 2>/dev/null
owner="$(stat -f %u "$here")"
chown "$owner" "$out/user-driver.log" "$out/user-airportd.log"
echo "saved: $(wc -l < "$out/user-driver.log") driver lines, $(wc -l < "$out/user-airportd.log") airportd lines"
