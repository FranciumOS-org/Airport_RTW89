#!/bin/sh
# macOS: double-click to set up AirPort_RTW89 in your OpenCore EFI (setup.py).
cd "$(dirname "$0")" || exit 1
if ! python3 -c '' 2>/dev/null; then
    echo "Python 3 is needed. Install Apple's Command Line Tools: xcode-select --install"
    echo "then double-click setup.command again."
    printf 'Press Enter to close. '; read -r _
    exit 1
fi
exec python3 setup.py
