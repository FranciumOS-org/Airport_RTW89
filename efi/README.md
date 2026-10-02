# The old Wi-Fi stack in the EFI

For the card to show up as Wi-Fi in macOS (menu, System Settings), the kext has
to be a driver for Apple's IO80211 family as it was up to Ventura. Sonoma and
later ship a different one, so OpenCore puts the old one back:

| What | Where it goes | From |
|---|---|---|
| `IOSkywalkFamily.kext` (Ventura's) | `EFI/OC/Kexts`, `Kernel -> Add` | OpenCore Legacy Patcher, `payloads/Kexts/Wifi/IOSkywalkFamily-v1.2.0.zip` |
| `IO80211FamilyLegacy.kext` | `EFI/OC/Kexts`, `Kernel -> Add` | same folder, `IO80211FamilyLegacy-v1.0.0.zip` |
| `AMFIPass.kext` 1.4.1 | `EFI/OC/Kexts`, `Kernel -> Add`, after Lilu | `payloads/Kexts/Acidanthera/AMFIPass-v1.4.1-RELEASE.zip` |
| block of `com.apple.iokit.IOSkywalkFamily` | `Kernel -> Block`, strategy Exclude | |

All four entries have MinKernel 23.0.0 (Sonoma). The `AirPortBrcmNIC.kext`
plugin inside `IO80211FamilyLegacy.kext` is for Broadcom cards and is not
added. `SecureBootModel` must be `Disabled`.

The kexts are not in the repository. Unpack the three archives into `efi/kit/`
so that `efi/kit/IOSkywalkFamily.kext` and so on exist.

## Applying it

```sh
sudo efi/apply.sh
```

mounts the EFI partition, saves `config.plist` as `config.plist.pre-wifistack`
next to it, copies the kexts and adds the entries (`efi/prepare.py`, which
touches nothing else and can be run on a copy first to see the result). Then
restart.

## Undoing it

```sh
sudo efi/revert.sh
```

puts the saved config back. If macOS does not start: boot Windows, open an
administrator command prompt and

```
mountvol S: /S
copy /Y S:\EFI\OC\config.plist.pre-wifistack S:\EFI\OC\config.plist
```

## After the restart

`kmutil showloaded | grep -i -E 'skywalk|80211|amfipass'` should list
`com.apple.iokit.IOSkywalkFamily`, `com.apple.iokit.IO80211FamilyLegacy` and
`com.dhinakg.AMFIPass`.

## OpenCore on the internal EFI partition

On the TUF A15 OpenCore boots from a USB stick that macOS cannot read (the
mount fails with an I/O error), so macOS cannot update it. A kext that depends
on the injected family has to be injected too (`tools/deptest.sh` showed that
`kmutil` will not load one from the running system), which means the EFI gets
a new build for every test of the native driver's front. Hence a second copy
of OpenCore where macOS can write:

```sh
sudo efi/install_internal.sh
```

copies `EFI/OC` from the backup on the Windows data drive to the internal EFI
partition, with the stick's config (Wi-Fi stack added) and the three kexts. It
writes only `EFI/OC`; Windows' and Ubuntu's files are left alone. The stick
stays as it is: booting from it is the way back if the internal copy breaks.

The firmware then needs a boot entry for `\EFI\OC\OpenCore.efi` on that
partition. Either in the firmware setup (F2, Advanced Mode, Boot, Add New Boot
Option, pick the internal EFI partition and that file), or from an
administrator command prompt in Windows:

```
bcdedit /copy {bootmgr} /d "OpenCore internal"
bcdedit /set {the-id-it-printed} path \EFI\OC\OpenCore.efi
```

and choose it from the boot menu (Esc at power-on).
