# Shell installer (macOS)

The Kext Installer that comes with the kext is the easy way (see the main
[README](../README.md)). This is the same thing as shell scripts, without the
questions.

## Install or update

1. Put the three old Wi-Fi stack kexts (links in the main README) in
   `efi/kit/`: `IOSkywalkFamily.kext`, `IO80211FamilyLegacy.kext`,
   `AMFIPass.kext`.
2. Put `AirPort_RTW89.kext` in the folder above `efi/` (from a source checkout,
   `make bundle` builds it into `build/out/bundle/` instead).
3. Run:
   ```sh
   sudo efi/install.sh
   ```

It finds your OpenCore EFI (if there are several, it asks which; you can also
pass a volume UUID or mount path), checks there is room, saves your config as
`EFI/OC/config.plist.pre-airport-rtw89`, copies the kexts and adds the entries
in the right order with `efi/configure.py`. Nothing else in the config changes.
Then restart.

## Switch it off

```sh
sudo efi/uninstall.sh
```

switches the entries off (the files stay). You can also copy
`config.plist.pre-airport-rtw89` over `config.plist`.

## Check after restarting

```sh
kmutil showloaded | grep -i -E 'skywalk|80211|amfipass|rtw89'
```

should list `IOSkywalkFamily`, `IO80211FamilyLegacy`, `AMFIPass`,
`com.rtw89.front` and `com.rtw89.driver`.
