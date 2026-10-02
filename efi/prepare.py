#!/usr/bin/env python3
"""Add the old Wi-Fi stack to an OpenCore config.plist.

Usage: python3 efi/prepare.py IN_CONFIG OUT_CONFIG

Kernel -> Add gets IOSkywalkFamily.kext, IO80211FamilyLegacy.kext and
AMFIPass.kext (after Lilu, which AMFIPass needs); Kernel -> Block gets an
entry that keeps Apple's own IOSkywalkFamily out, so the injected one can take
its place. Both only from Darwin 23 (Sonoma) on. Entries that are already
there are left as they are, so running it twice changes nothing. Nothing else
in the file is touched.
"""
import plistlib, sys

MIN_KERNEL = '23.0.0'
KEXTS = [   # in load order
    ('IOSkywalkFamily.kext', 'IOSkywalkFamily', 'Wi-Fi: IOSkywalkFamily from Ventura'),
    ('IO80211FamilyLegacy.kext', 'IO80211FamilyLegacy', 'Wi-Fi: IO80211Family from Ventura'),
    ('AMFIPass.kext', 'AMFIPass', 'Wi-Fi: AMFIPass'),
]
BLOCK_ID = 'com.apple.iokit.IOSkywalkFamily'

def main():
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, 'rb') as f:
        config = plistlib.load(f)
    kernel = config['Kernel']
    add, block = kernel['Add'], kernel['Block']
    changes = []

    paths = [e.get('BundlePath') for e in add]
    if 'Lilu.kext' not in paths:
        sys.exit('Lilu.kext is not in Kernel -> Add; AMFIPass needs it')
    for bundle, exe, comment in KEXTS:
        if bundle in paths:
            entry = add[paths.index(bundle)]
            if not entry.get('Enabled'):
                entry['Enabled'] = True
                changes.append('enabled the existing entry for ' + bundle)
            continue
        add.append({
            'Arch': 'x86_64', 'BundlePath': bundle, 'Comment': comment, 'Enabled': True,
            'ExecutablePath': 'Contents/MacOS/' + exe, 'MaxKernel': '', 'MinKernel': MIN_KERNEL,
            'PlistPath': 'Contents/Info.plist',
        })
        paths.append(bundle)
        changes.append('Kernel -> Add: ' + bundle)
    if paths.index('AMFIPass.kext') < paths.index('Lilu.kext'):
        sys.exit('AMFIPass.kext comes before Lilu.kext in Kernel -> Add; fix the order by hand')
    if paths.index('IO80211FamilyLegacy.kext') < paths.index('IOSkywalkFamily.kext'):
        sys.exit('IO80211FamilyLegacy.kext comes before IOSkywalkFamily.kext; fix the order by hand')

    for entry in block:
        if entry.get('Identifier') == BLOCK_ID:
            if not entry.get('Enabled'):
                entry['Enabled'] = True
                changes.append('enabled the existing block of ' + BLOCK_ID)
            break
    else:
        block.append({
            'Arch': 'x86_64', 'Comment': "Wi-Fi: Apple's IOSkywalkFamily makes way for Ventura's",
            'Enabled': True, 'Identifier': BLOCK_ID, 'MaxKernel': '', 'MinKernel': MIN_KERNEL,
            'Strategy': 'Exclude',
        })
        changes.append('Kernel -> Block: ' + BLOCK_ID)

    security = config.get('Misc', {}).get('Security', {})
    if security.get('SecureBootModel') != 'Disabled':
        print('note: Misc -> Security -> SecureBootModel is %r; the guides for this stack '
              'want Disabled' % security.get('SecureBootModel'))

    with open(dst, 'wb') as f:
        plistlib.dump(config, f, sort_keys=False)
    for c in changes:
        print('  ' + c)
    if not changes:
        print('  nothing to change: the old Wi-Fi stack is already in this config')

if __name__ == '__main__':
    main()
