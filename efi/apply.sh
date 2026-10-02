#!/bin/sh
# Put the old Wi-Fi stack (three kexts and one block entry) into the OpenCore EFI.
#
#   sudo efi/apply.sh [EFI partition, e.g. disk1s1]
#
# Only a person runs this. It copies IOSkywalkFamily.kext, IO80211FamilyLegacy.kext
# and AMFIPass.kext from efi/kit/ into EFI/OC/Kexts and adds them to config.plist
# (efi/prepare.py). The config as it was is kept next to it as
# config.plist.pre-wifistack; efi/revert.sh puts it back.
set -eu

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo efi/apply.sh" >&2; exit 1; }
here="$(cd "$(dirname "$0")" && pwd)"
kit="$here/kit"
for k in IOSkywalkFamily IO80211FamilyLegacy AMFIPass; do
    [ -f "$kit/$k.kext/Contents/MacOS/$k" ] || { echo "missing $kit/$k.kext (see efi/README.md)" >&2; exit 1; }
done

. "$here/find_efi.sh"
find_efi "${1:-}"
oc="$EFI_MOUNT/EFI/OC"
echo "OpenCore found at $oc"

backup="$oc/config.plist.pre-wifistack"
if [ -e "$backup" ]; then
    echo "keeping the existing backup $backup"
else
    cp "$oc/config.plist" "$backup"
    echo "saved the current config as $backup"
fi

tmp="$(mktemp -t config.plist)"
python3 "$here/prepare.py" "$oc/config.plist" "$tmp"
plutil -lint "$tmp" >/dev/null || { echo "the new config does not parse; nothing was changed" >&2; rm -f "$tmp"; exit 1; }

for k in IOSkywalkFamily IO80211FamilyLegacy AMFIPass; do
    if [ -d "$oc/Kexts/$k.kext" ]; then
        echo "$k.kext is already in EFI/OC/Kexts, left as it is"
    else
        cp -R "$kit/$k.kext" "$oc/Kexts/"
        echo "copied $k.kext"
    fi
done
# no AppleDouble files on the FAT volume: OpenCore would try to read them
find "$oc/Kexts" -name '._*' -delete 2>/dev/null || true

cp "$tmp" "$oc/config.plist"
rm -f "$tmp"
sync
echo "done. Restart to boot with the old Wi-Fi stack."
echo "to undo: sudo efi/revert.sh   (or, from Windows, copy config.plist.pre-wifistack over config.plist)"
