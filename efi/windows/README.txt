Old Wi-Fi stack for the OpenCore stick, applied from Windows
=============================================================

This folder was put on the internal EFI partition from macOS, because macOS
cannot read the OpenCore USB stick.

In Windows, with the OpenCore stick plugged in, open PowerShell as
administrator and run:

    mountvol S: /S
    powershell -ExecutionPolicy Bypass -File S:\rtw89-wifistack\apply.ps1
    mountvol S: /D

Then eject the stick safely and restart into macOS.

To undo: on the stick, in EFI\OC, copy config.plist.pre-wifistack over
config.plist.

This folder can be deleted afterwards.
