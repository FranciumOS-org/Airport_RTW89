#!/bin/sh
# Install AirPort_RTW89 into the OpenCore EFI: the old Wi-Fi stack (three kexts
# from OpenCore Legacy Patcher, see README), the front and the driver, and their
# config.plist entries (efi/configure.py). Run it again to update.
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

# ours: from a release download (Kexts/), else from a build (build/out/)
ours=
for d in "$root/Kexts" "$root/build/out"; do
    if [ -f "$d/AirPort_RTW89.kext/Contents/MacOS/AirPort_RTW89" ] &&
       [ -f "$d/AirPortRTW89Front.kext/Contents/MacOS/AirPortRTW89Front" ]; then
        ours="$d"
        break
    fi
done
[ -n "$ours" ] || { echo "AirPort_RTW89.kext and AirPortRTW89Front.kext not found in Kexts/ or build/out/ (run: make kext front)" >&2; exit 1; }
for k in IOSkywalkFamily IO80211FamilyLegacy AMFIPass; do
    [ -f "$kit/$k.kext/Contents/MacOS/$k" ] || {
        echo "missing $kit/$k.kext: download it as the README says and put it there" >&2; exit 1; }
done

. "$here/find_efi.sh"
find_efi "${1:-}"
oc="$EFI_MOUNT/EFI/OC"
echo "OpenCore: $oc ($EFI_PART)"

tmp="$(mktemp -t config.plist)"
trap 'rm -f "$tmp"' EXIT
python3 "$here/configure.py" install "$oc/config.plist" "$tmp"
plutil -lint "$tmp" >/dev/null || { echo "the new config does not parse; nothing was changed" >&2; exit 1; }

# about 6 MB go in (the Broadcom plugin inside IO80211FamilyLegacy is left out)
free_kb="$(df -k "$EFI_MOUNT" | awk 'NR == 2 { print $4 }')"
[ "$free_kb" -ge 8192 ] || { echo "only ${free_kb} KB free on the EFI volume; 8 MB wanted. Nothing was changed." >&2; exit 1; }

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
for k in AirPortRTW89Front AirPort_RTW89; do
    rm -rf "$oc/Kexts/$k.kext"
    cp -R -X "$ours/$k.kext" "$oc/Kexts/"
    echo "  copied $k.kext ($(shasum "$oc/Kexts/$k.kext/Contents/MacOS/$k" | cut -c1-12))"
done
# no AppleDouble files on the FAT volume: OpenCore would try to read them
find "$oc/Kexts" -name '._*' -delete 2>/dev/null || true

cp "$tmp" "$oc/config.plist"
sync
echo "done. Restart for it to take effect."
echo "to switch it off: sudo efi/uninstall.sh   (or boot from another OpenCore copy without it)"
