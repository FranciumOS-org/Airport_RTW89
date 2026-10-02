#!/bin/sh
# Put the front kext (build/out/AirPortRTW89Front.kext, make front) into the
# OpenCore copy on the internal EFI partition and add it to config.plist, after
# IO80211FamilyLegacy. It takes effect at the next restart.
#
#   sudo efi/install_front.sh            install or update
#   sudo efi/install_front.sh --disable  keep the files, switch the entry off
#
# Only a person runs this. The USB stick's OpenCore does not have the front:
# booting from the stick is the way back if this one stops macOS starting.
set -eu

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo efi/install_front.sh" >&2; exit 1; }
here="$(cd "$(dirname "$0")" && pwd)"
src="$here/../build/out/AirPortRTW89Front.kext"
enable=True
[ "${1:-}" = "--disable" ] && enable=False
[ -f "$src/Contents/MacOS/AirPortRTW89Front" ] || { echo "no kext at $src; run: make front" >&2; exit 1; }

part=disk1s1
perl -e 'alarm 90; exec @ARGV' diskutil mount "$part" >/dev/null
mp="$(diskutil info "$part" | sed -n 's/^ *Mount Point: *//p')"
oc="$mp/EFI/OC"
[ -f "$oc/config.plist" ] || { echo "no OpenCore at $oc (see efi/install_internal.sh)" >&2; exit 1; }

export COPYFILE_DISABLE=1
rm -rf "$oc/Kexts/AirPortRTW89Front.kext"
cp -R -X "$src" "$oc/Kexts/"
find "$oc/Kexts/AirPortRTW89Front.kext" -name '._*' -delete 2>/dev/null || true

tmp="$(mktemp -t config.plist)"
python3 - "$oc/config.plist" "$tmp" "$enable" <<'PY'
import plistlib, sys
src, dst, enable = sys.argv[1], sys.argv[2], sys.argv[3] == 'True'
c = plistlib.load(open(src, 'rb'))
add = c['Kernel']['Add']
paths = [e.get('BundlePath') for e in add]
if 'IO80211FamilyLegacy.kext' not in paths:
    sys.exit('IO80211FamilyLegacy.kext is not in this config: the front cannot link without it')
name = 'AirPortRTW89Front.kext'
if name in paths:
    add[paths.index(name)]['Enabled'] = enable
else:
    add.insert(paths.index('IO80211FamilyLegacy.kext') + 1, {
        'Arch': 'x86_64', 'BundlePath': name, 'Comment': 'Wi-Fi: RTL8852BE front (AirPort_RTW89)',
        'Enabled': enable, 'ExecutablePath': 'Contents/MacOS/AirPortRTW89Front', 'MaxKernel': '',
        'MinKernel': '23.0.0', 'PlistPath': 'Contents/Info.plist',
    })
plistlib.dump(c, open(dst, 'wb'), sort_keys=False)
print('  Kernel -> Add: %s %s' % (name, 'enabled' if enable else 'disabled'))
PY
plutil -lint "$tmp" >/dev/null
cp "$oc/config.plist" "$oc/config.plist.before-front"
cp "$tmp" "$oc/config.plist"
rm -f "$tmp"
sync
echo "done ($(shasum "$oc/Kexts/AirPortRTW89Front.kext/Contents/MacOS/AirPortRTW89Front" | cut -c1-12)). Restart for it to take effect."
