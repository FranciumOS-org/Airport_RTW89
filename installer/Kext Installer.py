#!/usr/bin/env python3
"""Set up AirPort_RTW89 in an OpenCore EFI, from macOS, Windows or Linux.

    macOS:    double-click "Kext Installer.command"
    Windows:  double-click "Kext Installer.cmd"
    Linux:    sh "Kext Installer.sh"
(each installs Python 3 first if it is missing), or: python3 "Kext Installer.py"

In the release this file sits next to AirPort_RTW89.kext and has efi/configure.py
built in (make release); in the source tree it uses efi/configure.py.

It asks for the EFI's config.plist (drag it into the window, or press Enter to
look for it on mounted volumes), then, after showing what it will do and
asking:
  - copies AirPort_RTW89.kext into EFI/OC/Kexts, and the three kexts of the old
    Wi-Fi stack if they are not there yet (you download those: see README);
  - removes the Broadcom plugin inside IO80211FamilyLegacy.kext, and the
    separate AirPortRTW89Front.kext of early versions;
  - saves the config, then puts the Kernel -> Add entries in the order they
    must load in, adds the Kernel -> Block entry for Apple's IOSkywalkFamily,
    and, if you agree, sets SecureBootModel to Disabled and adds -amfipassbeta
    for macOS Tahoe.
Nothing else in the config is changed.
"""
import datetime
import os
import platform
import plistlib
import shutil
import string
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# efi/configure.py: the config.plist changes (built into the released copy)
sys.path[:0] = [os.path.join(HERE, 'efi'), os.path.join(HERE, '..', 'efi')]
import configure  # noqa: E402  # BUILT-IN CONFIGURE

OCLP = ('https://github.com/dortania/OpenCore-Legacy-Patcher/raw/'
        'd9604c36a432eaf243ea18659ff4d208187452d7/payloads/Kexts')
STACK = [  # name, where to download it
    ('IOSkywalkFamily', OCLP + '/Wifi/IOSkywalkFamily-v1.2.0.zip'),
    ('IO80211FamilyLegacy', OCLP + '/Wifi/IO80211FamilyLegacy-v1.0.0.zip'),
    ('AMFIPass', OCLP + '/Acidanthera/AMFIPass-v1.4.1-RELEASE.zip'),
]
FRONT_PLUGIN = os.path.join('Contents', 'PlugIns', 'AirPortRTW89Front.kext')
BRCM_PLUGIN = os.path.join('Contents', 'PlugIns', 'AirPortBrcmNIC.kext')
NVRAM_GUID = '7C436110-AB2A-4BBB-A880-FE41995C9F82'
WINDOWS = os.name == 'nt'


def say(text=''):
    print(text, flush=True)


def read(prompt):
    try:
        return input(prompt)
    except EOFError:
        stop('no answer')


def ask(question, default=True):
    hint = 'Y/n' if default else 'y/N'
    while True:
        answer = read('%s [%s] ' % (question, hint)).strip().lower()
        if not answer:
            return default
        if answer in ('y', 'yes'):
            return True
        if answer in ('n', 'no'):
            return False


def stop(text):
    say()
    say('Stopped: ' + text)
    say('Nothing was changed.' if not CHANGED else '')
    pause()
    sys.exit(1)


def pause():
    # a double-clicked window would close before the text can be read
    if sys.stdin.isatty():
        try:
            input('Press Enter to close. ')
        except EOFError:
            pass


CHANGED = False


def clean_path(text):
    """A path typed, pasted or dragged into the terminal."""
    text = text.strip()
    if len(text) >= 2 and text[0] == text[-1] and text[0] in '"\'':
        text = text[1:-1]
    elif not WINDOWS:
        text = text.replace('\\ ', ' ')     # macOS and Linux escape dragged spaces
    return os.path.expanduser(text)


def candidate_configs():
    """config.plist files of OpenCore on the mounted volumes."""
    roots = []
    if WINDOWS:
        roots = ['%s:\\' % d for d in string.ascii_uppercase]
    elif sys.platform == 'darwin':
        roots = [os.path.join('/Volumes', v) for v in sorted(os.listdir('/Volumes'))]
    else:
        roots = ['/boot', '/boot/efi', '/efi']
        for base in ('/mnt', '/media', '/run/media'):
            for sub, dirs, _ in os.walk(base):
                roots += [os.path.join(sub, d) for d in dirs]
                if sub.count(os.sep) - base.count(os.sep) >= 1:
                    dirs[:] = []
    found = []
    for root in roots:
        path = os.path.join(root, 'EFI', 'OC', 'config.plist')
        try:
            if os.path.isfile(path):
                found.append(path)
        except OSError:
            pass
    return found


def mount_hint():
    if WINDOWS:
        return ('mount the EFI partition first: in a Command Prompt run as administrator, '
                '"mountvol S: /s", then use S:\\EFI\\OC\\config.plist')
    if sys.platform == 'darwin':
        return ('mount the EFI partition first: "sudo diskutil mount EFI" (or with '
                'MountEFI / Hackintool), then drag EFI/OC/config.plist in here')
    return 'mount the EFI partition first (often /boot/efi), then give EFI/OC/config.plist'


def try_mount():
    """Mount the EFI partition(s) if the user agrees; True if one was mounted."""
    if WINDOWS:
        letter = next((d for d in 'SRQPOT' if not os.path.exists('%s:\\' % d)), None)
        if not letter or not ask('Mount the EFI partition as %s:?' % letter):
            return False
        return subprocess.call(['mountvol', letter + ':', '/s']) == 0
    if sys.platform != 'darwin':
        return False
    try:
        listing = subprocess.check_output(['diskutil', 'list'], universal_newlines=True)
    except (OSError, subprocess.CalledProcessError):
        return False
    parts = [line.split()[-1] for line in listing.splitlines()
             if len(line.split()) > 2 and line.split()[1] == 'EFI']
    if not parts or not ask('Mount the EFI partition(s) %s to look there?' % ', '.join(parts)):
        return False
    mounted = False
    for part in parts:
        mounted |= subprocess.call(['diskutil', 'mount', part]) == 0
    return mounted


def choose_config():
    say('Drag your OpenCore config.plist into this window and press Enter,')
    typed = read('or just press Enter to look for it on mounted volumes: ')
    if typed.strip():
        path = clean_path(typed)
        if not os.path.isfile(path):
            stop('no file at %s' % path)
        return path
    found = candidate_configs()
    if not found and try_mount():
        found = candidate_configs()
    if not found:
        stop('no EFI/OC/config.plist on a mounted volume; ' + mount_hint())
    if len(found) == 1:
        say('Found ' + found[0])
        if not ask('Use it?'):
            stop('give the path to config.plist instead')
        return found[0]
    say('OpenCore is on more than one volume:')
    for k, path in enumerate(found, 1):
        say('  %d. %s' % (k, path))
    while True:
        pick = read('Which one does this machine boot from? (number) ').strip()
        if pick.isdigit() and 1 <= int(pick) <= len(found):
            return found[int(pick) - 1]


def bundle_version(kext):
    try:
        with open(os.path.join(kext, 'Contents', 'Info.plist'), 'rb') as f:
            return plistlib.load(f).get('CFBundleShortVersionString', '?')
    except (OSError, ValueError):
        return '?'


def is_kext(path, name):
    return os.path.isfile(os.path.join(path, 'Contents', 'MacOS', name))


def find_kext(name, extra=()):
    """Look for NAME.kext next to this script, in its efi/kit, in Downloads
    (also one folder down, where unzipping puts it), and in EXTRA."""
    downloads = os.path.join(os.path.expanduser('~'), 'Downloads')
    places = list(extra) + [HERE, os.path.join(HERE, 'efi', 'kit'),
                            os.path.join(HERE, '..', 'efi', 'kit'),
                            os.path.join(HERE, '..', 'build', 'out', 'bundle'), downloads]
    for place in places:
        if not os.path.isdir(place):
            continue
        direct = os.path.join(place, name + '.kext')
        if is_kext(direct, name):
            return direct
        if place == downloads:
            for sub in sorted(os.listdir(place)):
                inside = os.path.join(place, sub, name + '.kext')
                if is_kext(inside, name):
                    return inside
    return None


def tree_kb(path):
    total = 0
    for root, _, files in os.walk(path):
        for f in files:
            try:
                total += os.path.getsize(os.path.join(root, f))
            except OSError:
                pass
    return total // 1024 + 1


def copy_kext(src, dst, skip=None):
    """Copy a kext onto the FAT volume: no macOS metadata files (._*), which
    OpenCore would try to read."""
    if os.path.exists(dst):
        shutil.rmtree(dst)
    ignore = None
    if skip:
        ignore = lambda d, names: [n for n in names if os.path.join(d, n) == os.path.join(src, skip)]
    shutil.copytree(src, dst, ignore=ignore)
    for root, dirs, files in os.walk(dst):
        for f in files:
            if f.startswith('._') or f == '.DS_Store':
                os.remove(os.path.join(root, f))


def boot_args(config):
    nv = config.setdefault('NVRAM', {}).setdefault('Add', {}).setdefault(NVRAM_GUID, {})
    return nv, nv.get('boot-args', '')


def need_root(path):
    """On macOS and Linux an EFI volume is often root's: start again with sudo."""
    if WINDOWS or os.access(path, os.W_OK):
        return
    if os.geteuid() != 0:
        say('Writing to %s needs administrator rights; asking for your password (sudo).' % path)
        os.execvp('sudo', ['sudo', sys.executable, os.path.abspath(__file__), '--config', CONFIG])
    stop('cannot write to %s' % path)


def main():
    global CHANGED, CONFIG
    say('AirPort_RTW89 Kext Installer (%s, Python %s)' % (platform.system(), platform.python_version()))
    say()
    if len(sys.argv) == 3 and sys.argv[1] == '--config':
        CONFIG = sys.argv[2]
    else:
        CONFIG = choose_config()
    oc = os.path.dirname(os.path.abspath(CONFIG))
    kexts = os.path.join(oc, 'Kexts')
    if not os.path.isdir(kexts):
        stop('no Kexts folder next to %s: is this OpenCore\'s EFI/OC/config.plist?' % CONFIG)
    need_root(kexts)

    with open(CONFIG, 'rb') as f:
        try:
            config = plistlib.load(f)
        except Exception as err:
            stop('%s does not read as a plist (%s)' % (CONFIG, err))
    if 'Kernel' not in config or 'Add' not in config['Kernel']:
        stop('%s has no Kernel -> Add: not an OpenCore config?' % CONFIG)
    if not any(e.get('BundlePath') == 'Lilu.kext' for e in config['Kernel']['Add']):
        stop('Lilu.kext is not in Kernel -> Add; it is needed (AMFIPass is a Lilu plugin)')

    # what goes in
    ours = find_kext('AirPort_RTW89')
    if not ours or not is_kext(os.path.join(ours, FRONT_PLUGIN), 'AirPortRTW89Front'):
        say('AirPort_RTW89.kext (from the release zip) is not next to this script or in Downloads.')
        ours = clean_path(read('Drag AirPort_RTW89.kext in here and press Enter: '))
        if not is_kext(ours, 'AirPort_RTW89') or \
                not is_kext(os.path.join(ours, FRONT_PLUGIN), 'AirPortRTW89Front'):
            stop('%s is not AirPort_RTW89.kext 0.2.0 or later (the front must be inside it)' % ours)
    stack = {}
    missing = []
    for name, url in STACK:
        there = os.path.join(kexts, name + '.kext')
        if is_kext(there, name):
            stack[name] = None          # already in the EFI: left as it is
            continue
        found = find_kext(name)
        if found:
            stack[name] = found
        else:
            missing.append((name, url))
    if missing:
        say('These kexts of the old Wi-Fi stack are not in the EFI, next to this script or in Downloads:')
        for name, url in missing:
            say('  %s.kext: %s' % (name, url))
        stop('download them (each link is a zip with the kext inside), unzip them next to '
             'this script or in Downloads, and run the Kext Installer again')

    # what it will do
    plan = ['copy AirPort_RTW89.kext %s (%s) into %s' % (bundle_version(ours), ours, kexts)]
    for name, src in stack.items():
        plan.append('%s.kext: %s' % (name, 'already in the EFI, left as it is' if src is None
                                     else 'copy %s from %s' % (bundle_version(src), src)))
    brcm = os.path.join(kexts, 'IO80211FamilyLegacy.kext', BRCM_PLUGIN)
    if os.path.isdir(brcm) or stack.get('IO80211FamilyLegacy'):
        plan.append('leave out AirPortBrcmNIC.kext (inside IO80211FamilyLegacy; for Broadcom cards)')
    old_front = os.path.join(kexts, 'AirPortRTW89Front.kext')
    if os.path.isdir(old_front):
        plan.append('remove the separate AirPortRTW89Front.kext (inside the driver since 0.2.0)')
    plan.append('save config.plist, then set the Kernel -> Add order and the Kernel -> Block entry')
    say()
    say('Using %s' % CONFIG)
    for p in plan:
        say('  - ' + p)
    secure = configure.secure_boot_model(config)
    set_secure = secure != 'Disabled' and ask(
        'Misc -> Security -> SecureBootModel is %r; the old Wi-Fi stack needs Disabled. Set it?' % secure)
    nv, args = boot_args(config)
    tahoe = '-amfipassbeta' not in args.split() and ask(
        'Will this EFI boot macOS Tahoe (26)? AMFIPass then needs -amfipassbeta in boot-args.',
        default=False)
    say()
    if not ask('Go ahead?'):
        stop('you said no')

    # room on the volume: what comes in, less the copies it replaces
    need = tree_kb(ours) + 1024
    if os.path.isdir(os.path.join(kexts, 'AirPort_RTW89.kext')):
        need -= tree_kb(os.path.join(kexts, 'AirPort_RTW89.kext'))
    for name, src in stack.items():
        if src:
            need += tree_kb(src)
    free = shutil.disk_usage(kexts).free // 1024
    if free < need:
        stop('the EFI volume has %d KB free and this needs %d KB; make room on it first' % (free, need))

    # the config first, so a failure below leaves it as it was
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    backup = os.path.join(oc, 'config.plist.pre-airport-rtw89')
    if os.path.exists(backup):
        backup = os.path.join(oc, 'config.plist.before-kext-installer-' + stamp)
    shutil.copy2(CONFIG, backup)
    say('saved the config as %s' % backup)
    CHANGED = True

    copy_kext(ours, os.path.join(kexts, 'AirPort_RTW89.kext'))
    say('  copied AirPort_RTW89.kext')
    for name, src in stack.items():
        if src:
            copy_kext(src, os.path.join(kexts, name + '.kext'),
                      skip=BRCM_PLUGIN if name == 'IO80211FamilyLegacy' else None)
            say('  copied %s.kext' % name)
    if os.path.isdir(brcm):
        shutil.rmtree(brcm)
        say('  removed AirPortBrcmNIC.kext from IO80211FamilyLegacy.kext')
    if os.path.isdir(old_front):
        shutil.rmtree(old_front)
        say('  removed the separate AirPortRTW89Front.kext')

    changes = []
    if set_secure:
        config.setdefault('Misc', {}).setdefault('Security', {})['SecureBootModel'] = 'Disabled'
        changes.append('Misc -> Security -> SecureBootModel: Disabled')
    configure.install(config, changes)
    if tahoe:
        nv['boot-args'] = (args + ' -amfipassbeta').strip()
        changes.append('boot-args: added -amfipassbeta')
    tmp = CONFIG + '.airport-rtw89-new'
    with open(tmp, 'wb') as f:
        plistlib.dump(config, f, sort_keys=False)
    with open(tmp, 'rb') as f:
        plistlib.load(f)                # it must read back before it replaces the config
    os.replace(tmp, CONFIG)
    for c in changes or ['the config already had everything']:
        say('  ' + c)

    say()
    say('Done. Restart to use it; keep a USB stick that boots without this EFI at hand.')
    say('To go back: copy %s over config.plist.' % os.path.basename(backup))
    if not backup.endswith('pre-airport-rtw89'):
        say('(config.plist.pre-airport-rtw89 is the config from before the first install.)')
    pause()


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        say()
        stop('interrupted')
