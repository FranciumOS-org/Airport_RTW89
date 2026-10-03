#!/bin/sh
# Kept for the habit: the driver and the front are one kext now (the front in
# AirPort_RTW89.kext/Contents/PlugIns), installed by efi/install.sh. This
# builds nothing; run `make bundle` first.
#
#   sudo efi/install_front.sh            same as sudo efi/install.sh
#   sudo efi/install_front.sh --disable  same as sudo efi/uninstall.sh
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
if [ "${1:-}" = "--disable" ]; then
    shift
    exec "$here/uninstall.sh" "$@"
fi
exec "$here/install.sh" "$@"
