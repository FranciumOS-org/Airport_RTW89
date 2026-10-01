#!/usr/bin/env python3
"""List the symbols a partially linked object still needs from the kernel.

Usage: python3 tools/check_imports.py build/out/AirPort_RTW89.o [docs/kernel-imports.md]

Every undefined symbol must be something the running kernel has; anything else
is a function the compat layer still has to provide, and the script fails.
The kernel's symbol table is a superset of what kexts may link against, so a
pass here does not replace `kmutil libraries` on the finished kext.
"""
import subprocess
import sys
from pathlib import Path

KERNEL = Path("/System/Library/Kernels/kernel")


def symbols(path, flag):
    out = subprocess.run(["nm", flag, "-j", str(path)], capture_output=True, text=True)
    return {line.strip() for line in out.stdout.splitlines() if line.strip()}


def main():
    obj = Path(sys.argv[1])
    undefined = sorted(symbols(obj, "-u"))
    if not KERNEL.exists():
        print(f"{len(undefined)} undefined symbols; no {KERNEL} to check them against")
        return 0
    kernel = symbols(KERNEL, "-g")
    unresolved = [s for s in undefined if s not in kernel]

    if len(sys.argv) > 2:
        lines = ["# Kernel imports", "",
                 f"Symbols `{obj.name}` leaves for the kernel to resolve "
                 f"(`make link`): **{len(undefined)}**, unresolved: **{len(unresolved)}**.", ""]
        lines += [f"- `{s[1:]}`" + ("  **(not in this kernel)**" if s in unresolved else "")
                  for s in undefined]
        Path(sys.argv[2]).write_text("\n".join(lines) + "\n")

    print(f"  LINK {obj}: {len(undefined)} kernel imports, {len(unresolved)} unresolved")
    for s in unresolved:
        print(f"    missing: {s[1:]}")
    return 1 if unresolved else 0


if __name__ == "__main__":
    sys.exit(main())
