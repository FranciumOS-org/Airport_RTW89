#!/bin/sh
# Linux: sh "Kext Installer.sh" puts AirPort_RTW89 into an OpenCore EFI.
# Runs "Kext Installer.py" with Python 3, installing it with the system's
# package manager first if it is missing (asks for your password).
cd "$(dirname "$0")" || exit 1
if ! python3 -c 'import sys' >/dev/null 2>&1; then
    echo "Python 3 is needed to edit config.plist; installing it."
    if command -v apt-get >/dev/null 2>&1; then sudo apt-get install -y python3
    elif command -v dnf >/dev/null 2>&1; then sudo dnf install -y python3
    elif command -v pacman >/dev/null 2>&1; then sudo pacman -S --noconfirm python
    elif command -v zypper >/dev/null 2>&1; then sudo zypper install -y python3
    else echo "Install python3 with your package manager, then run this again."; exit 1
    fi
fi
exec python3 "Kext Installer.py"
