#!/bin/sh
# Install AirPort_RTW89 into the OpenCore EFI: the old Wi-Fi stack (three kexts
# from OpenCore Legacy Patcher, see README), the driver with the front inside it,
# and their config.plist entries (efi/configure.py). Run it again to update.
#
#   sudo efi/install.sh [EFI volume: a mounted path, a partition UUID or diskNsM]
#
# Without an argument it finds the volume with OpenCore on it; if there is
# more than one it asks which. The config as it was before the first install
# is kept as EFI/OC/config.plist.pre-airport-rtw89. Takes effect at the next
# restart; efi/uninstall.sh switches it all off again.
set -eu

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo $0" >&2; exit 1; }
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
kit="$here/kit"

# ours: AirPort_RTW89.kext with the front in Contents/PlugIns: from a release
# (put next to this folder's README), else from a build (make bundle)
ours=
for d in "$root" "$root/build/out/bundle"; do
    if [ -f "$d/AirPort_RTW89.kext/Contents/MacOS/AirPort_RTW89" ] &&
       [ -f "$d/AirPort_RTW89.kext/Contents/PlugIns/AirPortRTW89Front.kext/Contents/MacOS/AirPortRTW89Front" ]; then
        ours="$d/AirPort_RTW89.kext"
        break
    fi
done
[ -n "$ours" ] || { echo "AirPort_RTW89.kext not found: put it in $root (from the release zip), or build it: make bundle" >&2; exit 1; }

. "$here/find_efi.sh"
find_efi "${1:-}"
oc="$EFI_MOUNT/EFI/OC"
echo "OpenCore: $oc ($EFI_PART)"
# the old Wi-Fi stack: from efi/kit/, unless the EFI has it already
for k in IOSkywalkFamily IO80211FamilyLegacy AMFIPass; do
    [ -d "$oc/Kexts/$k.kext" ] || [ -f "$kit/$k.kext/Contents/MacOS/$k" ] || {
        echo "missing $kit/$k.kext: download it as the README says and put it there. Nothing was changed." >&2; exit 1; }
done

tmp="$(mktemp -t config.plist)"
trap 'rm -f "$tmp"' EXIT
python3 "$here/configure.py" install "$oc/config.plist" "$tmp"
plutil -lint "$tmp" >/dev/null || { echo "the new config does not parse; nothing was changed" >&2; exit 1; }

# the old Wi-Fi stack's kexts not there yet (without the Broadcom plugin
# inside IO80211FamilyLegacy, which is left out), then ours in place of any
# copies of them
EFI_EXTRA_KB=0
for k in IOSkywalkFamily IO80211FamilyLegacy AMFIPass; do
    [ -d "$oc/Kexts/$k.kext" ] && continue
    kb=$(du -sk "$kit/$k.kext" | cut -f1)
    [ -d "$kit/$k.kext/Contents/PlugIns" ] && kb=$((kb - $(du -sk "$kit/$k.kext/Contents/PlugIns" | cut -f1)))
    EFI_EXTRA_KB=$((EFI_EXTRA_KB + kb))
done
efi_room "$ours"

backup="$oc/config.plist.pre-airport-rtw89"
[ -e "$backup" ] || { cp "$oc/config.plist" "$backup"; echo "saved the config as it was: $backup"; }

export COPYFILE_DISABLE=1
for k in IOSkywalkFamily IO80211FamilyLegacy AMFIPass; do
    if [ -d "$oc/Kexts/$k.kext" ]; then
        echo "  $k.kext is already there, left as it is"
    else
        cp -R -X "$kit/$k.kext" "$oc/Kexts/"
        rm -rf "$oc/Kexts/$k.kext/Contents/PlugIns"
        echo "  copied $k.kext"
    fi
done
rm -rf "$oc/Kexts/AirPort_RTW89.kext"
cp -R -X "$ours" "$oc/Kexts/"
echo "  copied AirPort_RTW89.kext ($(shasum "$oc/Kexts/AirPort_RTW89.kext/Contents/MacOS/AirPort_RTW89" | cut -c1-12), commit $(cat "$ours/Contents/Resources/COMMIT" 2>/dev/null || echo '?'))"
# the front as a kext of its own, from early builds: its entry is off now
if [ -d "$oc/Kexts/AirPortRTW89Front.kext" ]; then
    rm -rf "$oc/Kexts/AirPortRTW89Front.kext"
    echo "  removed the separate AirPortRTW89Front.kext (it is inside the driver now)"
fi
# no AppleDouble files on the FAT volume: OpenCore would try to read them
find "$oc/Kexts" -name '._*' -delete 2>/dev/null || true

cp "$tmp" "$oc/config.plist"
sync
echo "done. Restart for it to take effect."
echo "to switch it off: sudo efi/uninstall.sh   (or boot from another OpenCore copy without it)"
