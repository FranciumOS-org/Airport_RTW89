# Scripts for the development machine only

The ASUS TUF A15 this port is developed on boots OpenCore from a USB stick
macOS cannot read, and from a second copy on the internal EFI partition that
these scripts set up and keep (`install_internal.sh`, `internal_efi.sh` with
that partition's UUID), with a way to apply a change from Windows when macOS
cannot write the EFI (`stage_for_windows.sh`, `windows/`, `prepare.py`).

None of this is needed on another machine: use `efi/install.sh`.

## OpenCore on the internal EFI partition

On the TUF A15 OpenCore boots from a USB stick that macOS cannot read (the
mount fails with an I/O error), so macOS cannot update it. A kext that depends
on the injected family has to be injected too (`tools/deptest.sh` showed that
`kmutil` will not load one from the running system), which means the EFI gets
a new build for every test of the native driver's front. Hence a second copy
of OpenCore where macOS can write:

```sh
sudo efi/tuf-a15/install_internal.sh
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

