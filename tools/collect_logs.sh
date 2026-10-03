#!/bin/sh
# Collect what a bug report needs, from this boot, into logs/ and one zip to
# attach:
#   logs/system.txt      macOS, the machine model, OpenCore, the Wi-Fi card's
#                        PCI IDs, the kexts loaded, rtw89ctl status (the link,
#                        not the list of networks around)
#   logs/driver.log      the driver's own log ring (everything since it loaded)
#   logs/kernel.log      the driver's lines in the unified log and dmesg
#   logs/airportd.log    macOS's Wi-Fi daemon, join-related lines only
#   logs/panics.txt      the latest panic reports that mention the driver
#   logs/AirPort_RTW89-report-<date>.zip   all of the above
#
#   sudo tools/collect_logs.sh
#
# No passwords: the driver never logs key material, and the daemon's lines are
# filtered to joins and failures. MAC addresses (the card's, access points')
# are cut to their first three bytes, the maker's part, since a full address
# can be looked up to a location; the account's and computer's names become
# <user>. Network names can still appear: read the files before posting them
# anywhere public.
set -eu
[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo $0" >&2; exit 1; }
here="$(cd "$(dirname "$0")/.." && pwd)"
out="$here/logs"
mkdir -p "$out"
ctl=
for c in "$here/tools/rtw89ctl" "$here/build/out/rtw89ctl"; do
    [ -x "$c" ] && { ctl="$c"; break; }
done

# aa:bb:cc:dd:ee:ff -> aa:bb:cc:xx:xx:xx, and the account's name, the words of
# its full name (as in "Name's iPhone") and the computer's name -> <user>
user="${SUDO_USER:-$(stat -f %Su "$here")}"
MASK_NAMES="$({ echo "$user"; id -F "$user" 2>/dev/null | tr -s ' \t' '\n'
                scutil --get ComputerName 2>/dev/null; } |
              awk 'length($0) >= 3' | sort -u | tr '\n' '\t')"
export MASK_NAMES
mask() {
    perl -pe 'BEGIN { @n = grep { length } split /\t/, $ENV{MASK_NAMES} }
              s/([0-9a-fA-F]{2}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2})(?::[0-9a-fA-F]{2}){3}/$1:xx:xx:xx/g;
              for my $w (sort { length $b <=> length $a } @n) { s/\Q$w\E/<user>/gi }'
}

# From the boot on. Not --last boot: after a wall clock adjustment that misses
# everything since (log show warns about it), while --start does not.
boot="$(sysctl -n kern.boottime | sed -n 's/^{ sec = \([0-9]*\),.*/\1/p')"
start="$(date -r "$boot" '+%Y-%m-%d %H:%M:%S')"

{
    echo "== collected $(date '+%Y-%m-%d %H:%M:%S'), boot $start"
    sw_vers
    echo "model: $(sysctl -n hw.model), CPU: $(sysctl -n machdep.cpu.brand_string)"
    echo "OpenCore: $(nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:opencore-version 2>/dev/null | cut -f2)"
    echo "boot-args: $(nvram boot-args 2>/dev/null | cut -f2)"
    echo "csr-active-config: $(nvram csr-active-config 2>/dev/null | cut -f2)"
    echo
    echo "== Realtek PCI devices (vendor 10ec)"
    ioreg -r -c IOPCIDevice -d 1 -l -w0 2>/dev/null | awk '
        function flush() { if (rt) printf "%s: %s\n", name, ids; rt = 0; ids = "" }
        /\+-o / { flush(); name = $0; sub(/.*\+-o /, "", name); sub(/  <class.*/, "", name) }
        /"vendor-id" = <ec100000>/ { rt = 1 }
        /"(device-id|subsystem-vendor-id|subsystem-id)" = </ {
            k = $0; sub(/^[^"]*"/, "", k); sub(/".*/, "", k)
            v = $0; sub(/.*= </, "", v); sub(/>.*/, "", v)
            ids = ids " " k "=" substr(v, 3, 2) substr(v, 1, 2)
        }
        END { flush() }' || true
    echo
    echo "== kexts"
    kmutil showloaded 2>/dev/null | grep -iE "rtw89|IO80211|Skywalk|AMFIPass|Lilu|Bluetooth|Realtek" || true
    echo
    echo "== rtw89ctl status"
    if [ -n "$ctl" ]; then
        # the link only: the networks around are left out
        "$ctl" status 2>&1 | sed '/network(s)$/,$d'
    else
        echo "rtw89ctl not found"
    fi
} 2>&1 | mask > "$out/system.txt"

if [ -n "$ctl" ]; then "$ctl" log 2>&1 | mask > "$out/driver.log" || true; else : > "$out/driver.log"; fi
{
    echo "== unified log"
    /usr/bin/log show --start "$start" --style compact \
        --predicate 'process == "kernel" AND (sender == "AirPort_RTW89" OR sender == "AirPortRTW89Front" OR eventMessage CONTAINS "AirPort_RTW89" OR eventMessage CONTAINS "AirPortRTW89" OR eventMessage CONTAINS "[rtw89")' 2>/dev/null
    # IOLog always reaches the kernel's message buffer; loaded from the EFI,
    # the driver's lines have not been reaching the unified log at all
    echo "== dmesg"
    dmesg 2>/dev/null | grep -E "AirPort_RTW89|AirPortRTW89|rtw89" || true
} | mask > "$out/kernel.log"
/usr/bin/log show --start "$start" --style compact \
    --predicate 'process == "airportd" AND (eventMessage CONTAINS[c] "join" OR eventMessage CONTAINS[c] "assoc" OR eventMessage CONTAINS[c] "fail")' \
    2>/dev/null | mask > "$out/airportd.log" || true
: > "$out/panics.txt"
for p in $(ls -t /Library/Logs/DiagnosticReports/*.panic 2>/dev/null | head -5); do
    grep -qiE "rtw89|AirPortRTW89" "$p" || continue
    { echo "== $p"; mask < "$p"; echo; } >> "$out/panics.txt"
done

zip="$out/AirPort_RTW89-report-$(date '+%Y%m%d-%H%M').zip"
(cd "$out" && zip -q "$zip" system.txt driver.log kernel.log airportd.log panics.txt)
owner="$(stat -f %u "$here")"
chown -R "$owner" "$out"
echo "saved in $out: $(wc -l < "$out/driver.log" | tr -d ' ') driver log lines, $(wc -l < "$out/kernel.log" | tr -d ' ') kernel lines, $(wc -l < "$out/airportd.log" | tr -d ' ') airportd lines, $(grep -c '^== /' "$out/panics.txt" || true) panic report(s)"
echo "attach: $zip"
echo "(MAC addresses are cut to their first half; network names can still be in it: look before posting it in public)"
