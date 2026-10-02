#!/bin/sh
# Put the driver (build/out/AirPort_RTW89.kext, make kext) into the OpenCore
# copy on the internal EFI partition and add it to config.plist, after the
# front. Wi-Fi then comes up at boot without tools/load.sh. It takes effect at
# the next restart.
#
#   sudo efi/install_driver.sh            install or update
#   sudo efi/install_driver.sh --disable  keep the files, switch the entry off
#
# Only a person runs this. The USB stick's OpenCore does not have the driver:
# booting from the stick is the way back if this one stops macOS starting.
# While the entry is enabled, tools/load.sh has nothing to load (the driver is
# already running); switch it off with --disable to go back to loading by hand.
set -eu

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo efi/install_driver.sh" >&2; exit 1; }
here="$(cd "$(dirname "$0")" && pwd)"
src="$here/../build/out/AirPort_RTW89.kext"
enable=True
[ "${1:-}" = "--disable" ] && enable=False
[ -f "$src/Contents/MacOS/AirPort_RTW89" ] || { echo "no kext at $src; run: make kext" >&2; exit 1; }

part=disk1s1
perl -e 'alarm 90; exec @ARGV' diskutil mount "$part" >/dev/null
mp="$(diskutil info "$part" | sed -n 's/^ *Mount Point: *//p')"
oc="$mp/EFI/OC"
[ -f "$oc/config.plist" ] || { echo "no OpenCore at $oc (see efi/install_internal.sh)" >&2; exit 1; }

export COPYFILE_DISABLE=1
if [ "$enable" = True ]; then
    rm -rf "$oc/Kexts/AirPort_RTW89.kext"
    cp -R -X "$src" "$oc/Kexts/"
    find "$oc/Kexts/AirPort_RTW89.kext" -name '._*' -delete 2>/dev/null || true
fi

tmp="$(mktemp -t config.plist)"
python3 - "$oc/config.plist" "$tmp" "$enable" <<'PY'
import plistlib, sys
src, dst, enable = sys.argv[1], sys.argv[2], sys.argv[3] == 'True'
c = plistlib.load(open(src, 'rb'))
add = c['Kernel']['Add']
paths = [e.get('BundlePath') for e in add]
front = 'AirPortRTW89Front.kext'
if front not in paths:
    sys.exit('%s is not in this config: install it first (efi/install_front.sh)' % front)
name = 'AirPort_RTW89.kext'
if name in paths:
    add[paths.index(name)]['Enabled'] = enable
else:
    add.insert(paths.index(front) + 1, {
        'Arch': 'x86_64', 'BundlePath': name, 'Comment': 'Wi-Fi: RTL8852BE driver (AirPort_RTW89)',
        'Enabled': enable, 'ExecutablePath': 'Contents/MacOS/AirPort_RTW89', 'MaxKernel': '',
        'MinKernel': '23.0.0', 'PlistPath': 'Contents/Info.plist',
    })
plistlib.dump(c, open(dst, 'wb'), sort_keys=False)
print('  Kernel -> Add: %s %s' % (name, 'enabled' if enable else 'disabled'))
PY
plutil -lint "$tmp" >/dev/null
cp "$oc/config.plist" "$oc/config.plist.before-driver"
cp "$tmp" "$oc/config.plist"
rm -f "$tmp"
sync
if [ "$enable" = True ]; then
    echo "done ($(shasum "$oc/Kexts/AirPort_RTW89.kext/Contents/MacOS/AirPort_RTW89" | cut -c1-12)). Restart for it to take effect."
else
    echo "done: entry switched off. Restart for it to take effect."
fi
