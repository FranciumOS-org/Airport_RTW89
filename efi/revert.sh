#!/bin/sh
# Undo efi/apply.sh: put back the config saved as config.plist.pre-wifistack.
#
#   sudo efi/revert.sh [EFI partition, e.g. disk1s1]
#
# The three kexts stay in EFI/OC/Kexts; without their config entries OpenCore
# ignores them.
set -eu

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo efi/revert.sh" >&2; exit 1; }
here="$(cd "$(dirname "$0")" && pwd)"
. "$here/find_efi.sh"
find_efi "${1:-}"
oc="$EFI_MOUNT/EFI/OC"
[ -f "$oc/config.plist.pre-wifistack" ] || { echo "no $oc/config.plist.pre-wifistack: nothing to put back" >&2; exit 1; }
cp "$oc/config.plist" "$oc/config.plist.with-wifistack"
cp "$oc/config.plist.pre-wifistack" "$oc/config.plist"
sync
echo "the previous config is back (the one just replaced is kept as config.plist.with-wifistack). Restart."
