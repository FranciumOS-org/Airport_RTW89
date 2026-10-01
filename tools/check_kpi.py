#!/usr/bin/env python3
"""Check that every symbol a built kext imports is exported by a library it declares.

Usage: python3 tools/check_kpi.py build/out/AirPort_RTW89.kext

`kmutil libraries -p` reports, for each undefined symbol it can place, the KPI
that exports it. Anything it does not report would fail at load time with
"unresolved symbol", so compare its list against the binary's own.
"""
import plistlib
import re
import subprocess
import sys
from pathlib import Path


def main():
    kext = Path(sys.argv[1])
    info = plistlib.loads((kext / "Contents/Info.plist").read_bytes())
    binary = kext / "Contents/MacOS" / info["CFBundleExecutable"]
    declared = set(info.get("OSBundleLibraries", {}))

    nm = subprocess.run(["nm", "-u", "-j", str(binary)], capture_output=True, text=True)
    wanted = {s.strip() for s in nm.stdout.splitlines() if s.strip()}

    km = subprocess.run(["kmutil", "libraries", "-p", str(kext)], capture_output=True, text=True)
    found = {}
    for m in re.finditer(r"^\s+(\S+) in \S+: (\S+) \(", km.stdout + km.stderr, re.M):
        found.setdefault(m.group(1), set()).add(m.group(2))

    # A C++ class keeps spare vtable slots named <Class>::_RESERVED<Class><n>().
    # When a later version of the class starts using a slot, that symbol is no
    # longer exported; the kext linker fills the subclass's slot from the
    # parent's vtable instead ("pad slot" patching). Such symbols are expected
    # for IONetworkController, whose slots 2-7 are in use on current systems.
    pad_slots = {s for s in wanted - set(found) if re.search(r"\d+_RESERVED", s)}

    missing = sorted(wanted - set(found) - pad_slots)
    undeclared = sorted(s for s in wanted & set(found) if not found[s] & declared)
    used = sorted({lib for s in wanted & set(found) for lib in found[s] & declared})

    print(f"  KPI  {len(wanted)} imports; libraries used: {', '.join(used)}")
    if pad_slots:
        print(f"    {len(pad_slots)} vtable pad slot(s) left for the kext linker to patch")
    for s in missing:
        print(f"    no KPI exports {s}")
    for s in undeclared:
        print(f"    {s} is in {', '.join(sorted(found[s]))}, not declared in OSBundleLibraries")
    return 1 if missing or undeclared else 0


if __name__ == "__main__":
    sys.exit(main())
