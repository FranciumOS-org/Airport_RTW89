#!/usr/bin/env python3
"""Add AirPort_RTW89 to an OpenCore config.plist, or switch it off again.

Usage: python3 efi/configure.py install|uninstall IN_CONFIG OUT_CONFIG

install: Kernel -> Add gets, in load order and after Lilu,
    IOSkywalkFamily.kext, IO80211FamilyLegacy.kext, AMFIPass.kext  (the old Wi-Fi stack)
    AirPort_RTW89.kext      (the driver)
    AirPort_RTW89.kext/Contents/PlugIns/AirPortRTW89Front.kext
                            (the IO80211 controller, links against the family)
and Kernel -> Block gets an entry that keeps Apple's own IOSkywalkFamily out,
all from Darwin 23 (Sonoma) on. Entries already there are switched on and
otherwise left alone, so running it twice changes nothing. An entry for the
front as a kext of its own (AirPortRTW89Front.kext, as early builds had it) is
switched off: it is inside the driver now.

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
FRONT = 'AirPort_RTW89.kext/Contents/PlugIns/AirPortRTW89Front.kext'
OURS = [
    ('AirPort_RTW89.kext', 'AirPort_RTW89', 'Wi-Fi: rtw89 driver (AirPort_RTW89)'),
    (FRONT, 'AirPortRTW89Front', 'Wi-Fi: rtw89 front (AirPort_RTW89)'),
]
OLD_FRONT = 'AirPortRTW89Front.kext'     # the front as a kext of its own
BRCM = 'IO80211FamilyLegacy.kext/Contents/PlugIns/AirPortBrcmNIC.kext'
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
    ours = []
    for bundle, exe, comment in STACK + OURS:
        if bundle in paths:
            e = add[paths.index(bundle)]
            if not e.get('Enabled'):
                e['Enabled'] = True
                changes.append('switched on ' + bundle)
        else:
            e = entry(bundle, exe, comment)
            changes.append('Kernel -> Add: ' + bundle)
        ours.append(e)

    # In load order right after Lilu: each must come after what it links
    # against (AMFIPass after Lilu, IO80211FamilyLegacy after IOSkywalkFamily,
    # the front after IO80211FamilyLegacy and after its own driver). OC Snapshot
    # sorts AirPort_RTW89 before IO80211FamilyLegacy, and OpenCore then cannot
    # inject the front ("Invalid Parameter").
    old_order = [e.get('BundlePath') for e in add]
    rest = [e for e in add if not any(e is o for o in ours)]
    lilu = next(k for k, e in enumerate(rest) if e.get('BundlePath') == 'Lilu.kext')
    add[:] = rest[:lilu + 1] + ours + rest[lilu + 1:]
    # entries that were there already and have moved
    new_order = [e.get('BundlePath') for e in add if e.get('BundlePath') in old_order]
    if new_order != old_order:
        changes.append('Kernel -> Add order: Lilu, ' +
                       ', '.join(b.split('/')[-1] for b, _, _ in STACK + OURS))

    for e in add:
        if e.get('BundlePath') == OLD_FRONT and e.get('Enabled'):
            e['Enabled'] = False
            changes.append('switched off ' + OLD_FRONT + ' (the front is inside the driver now)')
        if e.get('BundlePath') == BRCM and e.get('Enabled'):
            e['Enabled'] = False
            changes.append('switched off ' + BRCM + ' (for Broadcom cards)')

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

    if secure_boot_model(config) != 'Disabled':
        print('note: Misc -> Security -> SecureBootModel is %r; the old Wi-Fi stack needs '
              'Disabled' % secure_boot_model(config))


def secure_boot_model(config):
    return config.get('Misc', {}).get('Security', {}).get('SecureBootModel')


def uninstall(config, changes):
    kernel = config['Kernel']
    names = {b for b, _, _ in STACK + OURS} | {OLD_FRONT}
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
