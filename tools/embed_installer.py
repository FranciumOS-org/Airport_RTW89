#!/usr/bin/env python3
"""Write the released "Kext Installer.py": installer/Kext Installer.py with
efi/configure.py built in, so it runs on its own next to AirPort_RTW89.kext.

Usage: python3 tools/embed_installer.py OUT
"""
import sys

MARK = 'import configure  # noqa: E402  # BUILT-IN CONFIGURE'

src = open('installer/Kext Installer.py').read()
conf = open('efi/configure.py').read()
assert src.count(MARK) == 1, 'the import line to replace is missing'
built_in = ('import types  # noqa: E402\n'
            "configure = types.ModuleType('configure')  # efi/configure.py, built in:\n"
            'exec(compile(%r, "configure.py", "exec"), configure.__dict__)' % conf)
with open(sys.argv[1], 'w') as f:
    f.write(src.replace(MARK, built_in))
