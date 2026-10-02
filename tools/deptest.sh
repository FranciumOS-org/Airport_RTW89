#!/bin/sh
# After the old Wi-Fi stack is in the EFI and the machine has restarted: can a
# kext that depends on IO80211FamilyLegacy be loaded from the running system?
#
#   sudo tools/deptest.sh
#
# Loads build/out/RTW89DepTest.kext (make deptest), prints what happened and
# unloads it again. Only a person runs this.
set -u

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo tools/deptest.sh" >&2; exit 1; }
root="$(cd "$(dirname "$0")/.." && pwd)"
src="$root/build/out/RTW89DepTest.kext"
[ -d "$src" ] || { echo "no kext at $src; run: make deptest" >&2; exit 1; }

echo "--- Wi-Fi stack in the running kernel"
kmutil showloaded 2>/dev/null | grep -i -E 'skywalk|80211|amfipass' | awk '{print "   ", $6, $7}'
if ! kmutil showloaded 2>/dev/null | grep -q com.apple.iokit.IO80211FamilyLegacy; then
    echo "com.apple.iokit.IO80211FamilyLegacy is not loaded: the EFI change is not in effect"
    exit 1
fi

stage=/private/var/tmp/RTW89DepTest.stage
rm -rf "$stage"; mkdir -p "$stage"
cp -R "$src" "$stage/"
# copies of the injected kexts, for kmutil to resolve the dependency against
cp -R "$root/efi/kit/IO80211FamilyLegacy.kext" "$root/efi/kit/IOSkywalkFamily.kext" "$stage/" 2>/dev/null
chown -R root:wheel "$stage"; chmod -R go-w "$stage"
sync

try() {
    echo "--- $*"
    if "$@" 2>&1 | sed 's/^/    /' && kmutil showloaded 2>/dev/null | grep -q com.rtw89.deptest; then
        echo "=== LOADED with: $*"
        /usr/bin/log show --last 1m --style compact --predicate 'eventMessage CONTAINS "rtw89 deptest"' 2>/dev/null | tail -2
        kmutil unload -b com.rtw89.deptest 2>&1 | sed 's/^/    /'
        exit 0
    fi
}
try kmutil load -p "$stage/RTW89DepTest.kext"
try kmutil load -p "$stage/RTW89DepTest.kext" -r "$stage"
try kextutil -v 1 -d "$stage/IO80211FamilyLegacy.kext" -d "$stage/IOSkywalkFamily.kext" "$stage/RTW89DepTest.kext"
echo "=== NOT LOADED: a kext that depends on the injected family has to be injected by OpenCore too"
exit 1
