#!/usr/bin/env python3
"""Add AirPort_RTW89 to an OpenCore config.plist, or switch it off again.

Usage: python3 efi/configure.py install|uninstall IN_CONFIG OUT_CONFIG

install: Kernel -> Add gets, in load order and after Lilu,
    IOSkywalkFamily.kext, IO80211FamilyLegacy.kext, AMFIPass.kext  (the old Wi-Fi stack)
    AirPortRTW89Front.kext  (the IO80211 controller, links against the family)
    AirPort_RTW89.kext      (the driver)
and Kernel -> Block gets an entry that keeps Apple's own IOSkywalkFamily out,
all from Darwin 23 (Sonoma) on. Entries already there are switched on and
otherwise left alone, so running it twice changes nothing.

uninstall: the same entries are switched off (not removed), so the config is
as before as far as OpenCore is concerned, and install can switch them on.

Nothing else in the file is touched.
"""
import plistlib
import sys

MIN_KERNEL = '23.0.0'
# (bundle, executable, comment), in load order
STACK = [
    ('IOSkywalkFamily.kext', 'IOSkywalkFamily', 'Wi-Fi: IOSkywalkFamily from Ventura'),
    ('IO80211FamilyLegacy.kext', 'IO80211FamilyLegacy', 'Wi-Fi: IO80211Family from Ventura'),
    ('AMFIPass.kext', 'AMFIPass', 'Wi-Fi: AMFIPass'),
]
OURS = [
    ('AirPortRTW89Front.kext', 'AirPortRTW89Front', 'Wi-Fi: RTL8852BE front (AirPort_RTW89)'),
    ('AirPort_RTW89.kext', 'AirPort_RTW89', 'Wi-Fi: RTL8852BE driver (AirPort_RTW89)'),
]
BLOCK_ID = 'com.apple.iokit.IOSkywalkFamily'


def entry(bundle, exe, comment):
    return {
        'Arch': 'x86_64', 'BundlePath': bundle, 'Comment': comment, 'Enabled': True,
        'ExecutablePath': 'Contents/MacOS/' + exe, 'MaxKernel': '', 'MinKernel': MIN_KERNEL,
        'PlistPath': 'Contents/Info.plist',
    }


def install(config, changes):
    kernel = config['Kernel']
    add, block = kernel['Add'], kernel['Block']
    paths = [e.get('BundlePath') for e in add]
    if 'Lilu.kext' not in paths:
        sys.exit('Lilu.kext is not in Kernel -> Add; AMFIPass needs it')
    after = paths.index('Lilu.kext')
    for bundle, exe, comment in STACK + OURS:
        if bundle in paths:
            e = add[paths.index(bundle)]
            if not e.get('Enabled'):
                e['Enabled'] = True
                changes.append('switched on ' + bundle)
            after = max(after, paths.index(bundle))
            continue
        # right after the previous one of the list, so the order holds
        after += 1
        add.insert(after, entry(bundle, exe, comment))
        paths.insert(after, bundle)
        changes.append('Kernel -> Add: ' + bundle)
    # what each one links against must come first; the rest may be anywhere
    # (the driver waits for the front at boot)
    for before, later in (('Lilu.kext', 'AMFIPass.kext'),
                          ('IOSkywalkFamily.kext', 'IO80211FamilyLegacy.kext'),
                          ('IO80211FamilyLegacy.kext', 'AirPortRTW89Front.kext')):
        if paths.index(later) < paths.index(before):
            sys.exit('%s comes before %s in Kernel -> Add; it links against it: '
                     'fix the order by hand' % (later, before))

    for e in block:
        if e.get('Identifier') == BLOCK_ID:
            if not e.get('Enabled'):
                e['Enabled'] = True
                changes.append('switched on the block of ' + BLOCK_ID)
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
        print('note: Misc -> Security -> SecureBootModel is %r; the old Wi-Fi stack needs '
              'Disabled' % security.get('SecureBootModel'))


def uninstall(config, changes):
    kernel = config['Kernel']
    names = {b for b, _, _ in STACK + OURS}
    for e in kernel['Add']:
        if e.get('BundlePath') in names and e.get('Enabled'):
            e['Enabled'] = False
            changes.append('switched off ' + e['BundlePath'])
    for e in kernel['Block']:
        if e.get('Identifier') == BLOCK_ID and e.get('Enabled'):
            e['Enabled'] = False
            changes.append('switched off the block of ' + BLOCK_ID)


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ('install', 'uninstall'):
        sys.exit(__doc__)
    mode, src, dst = sys.argv[1:]
    with open(src, 'rb') as f:
        config = plistlib.load(f)
    changes = []
    (install if mode == 'install' else uninstall)(config, changes)
    with open(dst, 'wb') as f:
        plistlib.dump(config, f, sort_keys=False)
    for c in changes:
        print('  ' + c)
    if not changes:
        print('  nothing to change')


if __name__ == '__main__':
    main()
