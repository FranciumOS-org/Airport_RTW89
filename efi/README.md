# Installing into the OpenCore EFI

For the card to show up as Wi-Fi in macOS (menu, System Settings), the driver
has to work with Apple's IO80211 family as it was up to Ventura. Sonoma and
later ship a different one, so OpenCore puts the old one back, and with it
this project's kext:

| What | Where it goes | From |
|---|---|---|
| `IOSkywalkFamily.kext` (Ventura's) | `EFI/OC/Kexts`, `Kernel -> Add` | [OpenCore Legacy Patcher, IOSkywalkFamily-v1.2.0.zip](https://github.com/dortania/OpenCore-Legacy-Patcher/raw/d9604c36a432eaf243ea18659ff4d208187452d7/payloads/Kexts/Wifi/IOSkywalkFamily-v1.2.0.zip) |
| `IO80211FamilyLegacy.kext` | `EFI/OC/Kexts`, `Kernel -> Add` | [OpenCore Legacy Patcher, IO80211FamilyLegacy-v1.0.0.zip](https://github.com/dortania/OpenCore-Legacy-Patcher/raw/d9604c36a432eaf243ea18659ff4d208187452d7/payloads/Kexts/Wifi/IO80211FamilyLegacy-v1.0.0.zip) |
| `AMFIPass.kext` 1.4.1 | `EFI/OC/Kexts`, `Kernel -> Add`, after Lilu | [OpenCore Legacy Patcher, AMFIPass-v1.4.1-RELEASE.zip](https://github.com/dortania/OpenCore-Legacy-Patcher/raw/d9604c36a432eaf243ea18659ff4d208187452d7/payloads/Kexts/Acidanthera/AMFIPass-v1.4.1-RELEASE.zip) |
| block of `com.apple.iokit.IOSkywalkFamily` | `Kernel -> Block`, strategy Exclude | |
| `AirPort_RTW89.kext` | `EFI/OC/Kexts`, `Kernel -> Add`, after AMFIPass | this project |
| its plugin `Contents/PlugIns/AirPortRTW89Front.kext` | an entry of its own, right after the driver's | (inside the driver) |

All entries have MinKernel 23.0.0 (Sonoma). The `AirPortBrcmNIC.kext` plugin
inside `IO80211FamilyLegacy.kext` is for Broadcom cards and is left out.
`Misc -> Security -> SecureBootModel` must be `Disabled`.

Apple's kexts are not part of this project. Download the three archives (the
links in the table) and, for the installer, unpack them into `efi/kit/`, so that
`efi/kit/IOSkywalkFamily.kext`, `efi/kit/IO80211FamilyLegacy.kext` and
`efi/kit/AMFIPass.kext` exist.

## Installing

```sh
sudo efi/install.sh
```

finds the volume OpenCore is on (if there are several, for example an internal
copy and a USB stick, it lists them and asks for one: pass its UUID, as in
`sudo efi/install.sh 1234ABCD-...`, or its mount path), keeps the config as it
was as `EFI/OC/config.plist.pre-airport-rtw89`, copies the kexts and adds the
entries (`efi/configure.py`, which touches nothing else; it can be run on a
copy of a config first to see the result). Then restart. Running it again
updates the two kexts of this project.

Keep a way back before the first install: a USB stick with your OpenCore EFI
as it is now. If macOS does not start, boot from that.

## Switching it off

```sh
sudo efi/uninstall.sh
```

switches all those entries off (the files stay). From another system the
saved `config.plist.pre-airport-rtw89` can be copied over `config.plist`.

## After the restart

`kmutil showloaded | grep -i -E 'skywalk|80211|amfipass|rtw89'` should list
`com.apple.iokit.IOSkywalkFamily`, `com.apple.iokit.IO80211FamilyLegacy`,
`com.dhinakg.AMFIPass`, `com.rtw89.front` and `com.rtw89.driver`. The driver
waits up to 30 s at boot for the front, which waits for the injected family.

## Development

`make bundle` puts the driver and the front together
(`build/out/bundle/AirPort_RTW89.kext`) and `sudo efi/install.sh` installs it
(`efi/install_driver.sh` and `efi/install_front.sh` are the same thing now).
Early builds had the front as a kext of its own, `AirPortRTW89Front.kext`;
install.sh switches that entry off and removes it.

`efi/tuf-a15/` holds scripts for the machine this port is developed on only.
