#!/bin/sh
# For a machine whose OpenCore volume macOS cannot mount (here: a USB stick
# that only the firmware and Windows read): put everything the change needs
# where Windows can get at it, with a script that applies it there.
#
#   efi/stage_for_windows.sh [copy of the stick's config.plist] [folder to stage in]
#
# Defaults: the EFI backup on the Windows data drive, and a folder on the
# internal EFI partition (FAT, so both systems can use it; mount it first with
# sudo diskutil mount disk1s1). Only a person runs this. The Windows script
# refuses to touch a config.plist that differs from the copy given here.
set -eu

here="$(cd "$(dirname "$0")" && pwd)"
config="${1:-/Volumes/Extra Storage/mac development/setup/EFI-TUF-A15-working/EFI/OC/config.plist}"
dest="${2:-/Volumes/NO NAME/rtw89-wifistack}"
kit="$here/../kit"

[ -f "$config" ] || { echo "no config at $config" >&2; exit 1; }
[ -d "$(dirname "$dest")" ] || { echo "$(dirname "$dest") is not there: mount the volume first" >&2; exit 1; }
for k in IOSkywalkFamily IO80211FamilyLegacy AMFIPass; do
    [ -f "$kit/$k.kext/Contents/MacOS/$k" ] || { echo "missing $kit/$k.kext (see efi/README.md)" >&2; exit 1; }
done

# a copy of the stick's config that the Windows script saved here must survive
# (also when it is the very file this run reads)
keep="$(mktemp -t config.from-stick)"
if [ -f "$dest/config.from-stick.plist" ]; then
    cp "$dest/config.from-stick.plist" "$keep"
    [ "$config" = "$dest/config.from-stick.plist" ] && config="$keep"
fi
rm -rf "$dest"
mkdir -p "$dest/Kexts"
[ -s "$keep" ] && cp "$keep" "$dest/config.from-stick.plist"
export COPYFILE_DISABLE=1       # no ._ files on the FAT volume
for k in IOSkywalkFamily IO80211FamilyLegacy AMFIPass; do
    cp -R -X "$kit/$k.kext" "$dest/Kexts/"
    rm -rf "$dest/Kexts/$k.kext/Contents/PlugIns"
done
python3 "$here/prepare.py" "$config" "$dest/config.patched.plist"
plutil -lint "$dest/config.patched.plist" >/dev/null
before="$(shasum -a 256 "$config" | awk '{print $1}')"
sed "s/@EXPECTED@/$before/" "$here/windows/apply.ps1" > "$dest/apply.ps1"
cp "$here/windows/README.txt" "$dest/README.txt"
find "$dest" -name '._*' -delete 2>/dev/null || true
sync
echo "staged in $dest:"
ls "$dest" "$dest/Kexts"
echo "config it expects on the stick: sha256 $before"
