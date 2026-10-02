#!/bin/sh
# Put a copy of OpenCore on the internal EFI partition, next to the Windows and
# Ubuntu boot files, so that macOS itself can update it (it cannot read the USB
# stick OpenCore normally boots from). The stick is not touched and stays the
# way back in.
#
#   sudo efi/install_internal.sh [EFI backup folder] [config.plist to use] [partition]
#
# Defaults: the backup on the Windows data drive; the stick's config with the
# Wi-Fi stack added, as staged by efi/stage_for_windows.sh; the partition is found by its UUID.
# Only EFI/OC is written. EFI/BOOT, EFI/Microsoft and EFI/ubuntu are left
# alone, so the firmware needs a boot entry for \EFI\OC\OpenCore.efi.
# Only a person runs this.
set -eu

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo efi/install_internal.sh" >&2; exit 1; }
here="$(cd "$(dirname "$0")" && pwd)"
backup="${1:-/Volumes/Extra Storage/mac development/setup/EFI-TUF-A15-working}"
part="${3:-BC5C5FBD-D3C3-42BD-AE73-558AE9D9D5A5}"   # the internal EFI partition, by UUID (disk numbers change)
kit="$here/kit"

[ -f "$backup/EFI/OC/OpenCore.efi" ] || { echo "no EFI/OC/OpenCore.efi under $backup" >&2; exit 1; }
perl -e 'alarm 90; exec @ARGV' diskutil mount "$part" >/dev/null
mp="$(diskutil info "$part" | sed -n 's/^ *Mount Point: *//p')"
[ -d "$mp/EFI/Microsoft" ] || { echo "$part ($mp) does not look like the internal EFI partition" >&2; exit 1; }
config="${2:-$mp/rtw89-wifistack/config.patched.plist}"
[ -f "$config" ] || { echo "no config at $config" >&2; exit 1; }
plutil -lint "$config" >/dev/null
# keep our own copy: the staging folder is not for ever
mkdir -p "$kit"
cp "$config" "$kit/config.internal.plist"
config="$kit/config.internal.plist"

need_kb=40960
free_kb="$(df -k "$mp" | awk 'NR == 2 { print $4 }')"
[ -d "$mp/EFI/OC" ] || [ "$free_kb" -ge "$need_kb" ] || { echo "only ${free_kb} KB free on $mp" >&2; exit 1; }

export COPYFILE_DISABLE=1
oc="$mp/EFI/OC"
mkdir -p "$oc"
for d in ACPI Drivers Kexts Resources Tools; do
    [ -d "$backup/EFI/OC/$d" ] || continue
    rm -rf "$oc/$d"
    cp -R -X "$backup/EFI/OC/$d" "$oc/$d"
done
cp -X "$backup/EFI/OC/OpenCore.efi" "$oc/OpenCore.efi"
for k in IOSkywalkFamily IO80211FamilyLegacy AMFIPass; do
    rm -rf "$oc/Kexts/$k.kext"
    cp -R -X "$kit/$k.kext" "$oc/Kexts/"
    rm -rf "$oc/Kexts/$k.kext/Contents/PlugIns"
done
cp "$config" "$oc/config.plist"
find "$oc" -name '._*' -delete 2>/dev/null || true
sync

# every kext the config enables must be there
python3 - "$oc" <<'PY'
import os, plistlib, sys
oc = sys.argv[1]
c = plistlib.load(open(os.path.join(oc, 'config.plist'), 'rb'))
missing = []
for e in c['Kernel']['Add']:
    if not e.get('Enabled'):
        continue
    base = os.path.join(oc, 'Kexts', e['BundlePath'])
    for rel in (e.get('PlistPath'), e.get('ExecutablePath')):
        if rel and not os.path.isfile(os.path.join(base, rel)):
            missing.append(e['BundlePath'] + '/' + rel)
for a in c['ACPI']['Add']:
    if a.get('Enabled') and not os.path.isfile(os.path.join(oc, 'ACPI', a['Path'])):
        missing.append('ACPI/' + a['Path'])
for d in c['UEFI']['Drivers']:
    path = d['Path'] if isinstance(d, dict) else d
    enabled = d.get('Enabled', True) if isinstance(d, dict) else True
    if enabled and not os.path.isfile(os.path.join(oc, 'Drivers', path)):
        missing.append('Drivers/' + path)
if missing:
    print('MISSING, the config asks for these and they are not in the copy:')
    for m in missing:
        print('   ', m)
    sys.exit(1)
print('every kext, ACPI table and driver the config enables is in place')
PY

echo "OpenCore copied to $oc ($(du -sk "$oc" | awk '{print $1}') KB)."
echo "Next: a firmware boot entry for \\EFI\\OC\\OpenCore.efi on this partition (see efi/README.md)."
