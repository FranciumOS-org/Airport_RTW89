#!/bin/sh
# macOS: double-click to put AirPort_RTW89 into your OpenCore EFI.
# Runs "Kext Installer.py" with Python 3, installing Apple's Command Line Tools
# (which bring Python 3) first if there is no Python 3 yet.
cd "$(dirname "$0")" || exit 1

find_python() {
    for p in /opt/homebrew/bin/python3 /usr/local/bin/python3 \
             /Library/Frameworks/Python.framework/Versions/Current/bin/python3; do
        if [ -x "$p" ] && "$p" -c 'import sys' >/dev/null 2>&1; then PY="$p"; return 0; fi
    done
    # /usr/bin/python3 is only a stub until the Command Line Tools are there:
    # running it then just opens Apple's install dialog
    if xcode-select -p >/dev/null 2>&1 && /usr/bin/python3 -c 'import sys' >/dev/null 2>&1; then
        PY=/usr/bin/python3; return 0
    fi
    return 1
}

if ! find_python; then
    echo "Python 3 is needed to edit config.plist, and it is not installed."
    echo "Installing Apple's Command Line Tools, which include it: click Install in the window"
    echo "that opens and wait for it to finish (a few minutes). This continues by itself."
    xcode-select --install >/dev/null 2>&1
    tries=0
    until find_python; do
        tries=$((tries + 1))
        if [ $tries -gt 360 ]; then
            echo "Still no Python 3 after 30 minutes. Install the Command Line Tools"
            echo "(xcode-select --install) or Python from python.org, then try again."
            printf 'Press Enter to close. '; read -r _; exit 1
        fi
        sleep 5
    done
    echo "Python 3 is installed."
fi
exec "$PY" "Kext Installer.py"
