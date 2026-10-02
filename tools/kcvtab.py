#!/usr/bin/env python3
"""Dump C++ vtables of kernel classes from this Mac's kernel collections.

Usage: python3 tools/kcvtab.py CLASS [CLASS ...]
       e.g. python3 tools/kcvtab.py IO80211Controller AppleBCMWLANCore

Prints each class's virtual methods in slot order, demangled. The Wi-Fi family
(IO80211Family, IOSkywalkFamily) only exists inside the kernel collections,
with its symbols; this reads them from there. Slots a class leaves pure show
as ___cxa_pure_virtual: dump a subclass (Apple's own driver) to see what they
are. Reads /System/Library/KernelCollections only; needs no root.
"""
import bisect, mmap, struct, subprocess, sys

KCS = ['/System/Library/KernelCollections/BootKernelExtensions.kc',     # cache level 0
       '/System/Library/KernelCollections/SystemKernelExtensions.kc']   # cache level 1

class KC:
    def __init__(self, path):
        f = open(path, 'rb')
        self.m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        self.segs, self.entries = [], []
        for cmd, p, size in self.cmds(0):
            if cmd == 0x19:                 # LC_SEGMENT_64
                va, vs, fo, fs = struct.unpack_from('<QQQQ', self.m, p + 24)
                self.segs.append((va, fs, fo))
            elif cmd == 0x80000035:         # LC_FILESET_ENTRY
                vmaddr, fileoff, stroff, _r = struct.unpack_from('<QQII', self.m, p + 8)
                self.entries.append(fileoff)
        self.base = min(s[0] for s in self.segs)

    def cmds(self, off):
        magic, _c, _s, _t, ncmds = struct.unpack_from('<IiiII', self.m, off)
        assert magic == 0xfeedfacf
        p = off + 32
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<II', self.m, p)
            yield cmd, p, size
            p += size

    def read64(self, va):
        for sva, fs, fo in self.segs:
            if sva <= va < sva + fs:
                return struct.unpack_from('<Q', self.m, fo + va - sva)[0]
        return None

    def symbols(self):
        for fileoff in self.entries:
            for cmd, p, size in self.cmds(fileoff):
                if cmd != 0x2:              # LC_SYMTAB
                    continue
                symoff, nsyms, stroff, _ss = struct.unpack_from('<IIII', self.m, p + 8)
                for i in range(nsyms):
                    strx, typ, _sect, _desc, value = struct.unpack_from('<IBBHQ', self.m, symoff + 16 * i)
                    if typ & 0xe0 or (typ & 0x0e) != 0x0e:      # debug entries, undefined
                        continue
                    end = self.m.find(b'\0', stroff + strx)
                    yield value, self.m[stroff + strx:end].decode(errors='replace')

def main():
    kcs = [KC(p) for p in KCS]
    by_addr, by_name = {}, {}
    for kc in kcs:
        for value, name in kc.symbols():
            by_addr.setdefault(value, name)
            by_name.setdefault(name, (kc, value))
    addrs = sorted(by_addr)

    for cls in sys.argv[1:]:
        sym = '__ZTV%d%s' % (len(cls), cls)
        if sym not in by_name:
            print('## %s: no vtable symbol' % cls)
            continue
        kc, va = by_name[sym]
        end = addrs[bisect.bisect_right(addrs, va)]
        names = []
        for slot in range(va + 16, end, 8):     # after offset-to-top and RTTI
            raw = kc.read64(slot)
            if not raw:
                break
            # dyld_chained_ptr_64_kernel_cache_rebase: target:30, cacheLevel:2
            target = kcs[(raw >> 30) & 3].base + (raw & 0x3fffffff)
            names.append(by_addr.get(target, 'sub_%x' % target))
        out = subprocess.run(['c++filt'], input='\n'.join(names), capture_output=True, text=True).stdout
        print('## %s: %d slots' % (cls, len(names)))
        for i, n in enumerate(out.split('\n')[:len(names)]):
            print('%4d  %s' % (i, n))

if __name__ == '__main__':
    main()
