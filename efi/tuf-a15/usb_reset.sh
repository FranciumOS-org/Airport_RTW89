#!/bin/sh
# Switch SSDT-USB-Reset.aml off (or back on) in the internal EFI's config.plist,
# to test whether it is behind the USB/Bluetooth trouble (HCI timeouts, HoRNDIS
# dropping). Only the Enabled flag of that one ACPI entry changes; the config is
# saved first as config.plist.pre-usb-reset.
#
#   sudo efi/tuf-a15/usb_reset.sh off      # disable, then restart
#   sudo efi/tuf-a15/usb_reset.sh on       # enable again
#   sudo efi/tuf-a15/usb_reset.sh status
#
# Only a person runs this.
set -eu

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo efi/tuf-a15/usb_reset.sh off|on|status" >&2; exit 1; }
here="$(cd "$(dirname "$0")" && pwd)"
. "$here/internal_efi.sh"
internal_efi
config="$INTERNAL_OC/config.plist"

python3 - "$config" "${1:-status}" <<'PY'
import plistlib, shutil, sys
path, action = sys.argv[1], sys.argv[2]
if action not in ('off', 'on', 'status'):
    sys.exit('usage: usb_reset.sh off|on|status')
with open(path, 'rb') as f:
    cfg = plistlib.load(f)
entries = [a for a in cfg['ACPI']['Add'] if a.get('Path') == 'SSDT-USB-Reset.aml']
if not entries:
    sys.exit('no SSDT-USB-Reset.aml entry in ACPI -> Add')
print('SSDT-USB-Reset.aml is currently', 'enabled' if entries[0]['Enabled'] else 'disabled')
if action == 'status':
    sys.exit(0)
want = action == 'on'
if entries[0]['Enabled'] == want:
    print('nothing to change')
    sys.exit(0)
backup = path + '.pre-usb-reset'
shutil.copy2(path, backup)
for e in entries:
    e['Enabled'] = want
with open(path, 'wb') as f:
    plistlib.dump(cfg, f, sort_keys=False)
print('now', 'enabled' if want else 'disabled', '- previous config saved as', backup)
PY

plutil -lint "$config" >/dev/null && echo "config.plist is valid"
sync
echo "Restart for it to take effect."
