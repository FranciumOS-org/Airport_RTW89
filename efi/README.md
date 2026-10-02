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
