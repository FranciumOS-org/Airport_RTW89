# AirPort_RTW89

Native Wi-Fi on macOS for **Realtek rtw89 PCIe cards**, ported from the Linux
rtw89 driver. The card works from the Wi-Fi menu and System Settings like a
Mac's own.

> **Preview.** Works every day on the RTL8852BE it is developed on, and
> testers have it running on an RTL8852AE and an RTL8922DE. Other cards have
> not run it yet. Keep a USB stick that boots your current OpenCore EFI, in
> case macOS does not start.

## Supported cards

| Card | PCI ID (`10EC:`) | Wi-Fi | Status |
|---|---|---|---|
| RTL8852BE | `B852`, `B85B` | 6 | works |
| RTL8852AE | `8852`, `A85A` | 6 | works (tester) |
| RTL8922DE | `892D`, `882D`, `895D` | 7 | works (tester); 6 GHz and Wi-Fi 7 untested |
| RTL8852BTE | `B520` | 6 | untested |
| RTL8851BE | `B851` | 6 | untested |
| RTL8852CE | `C852` | 6E | untested |
| RTL8922AE | `8922`, `892B` | 7 | untested |

USB versions of these cards are not supported.

## What works

- **Networks:** WPA3, WPA2 (Personal and Enterprise), open, phone hotspots.
- **Speed:** Wi-Fi 6, up to 80 MHz (about 500 Mb/s measured). The driver
  prefers a network's 5 GHz side when its signal is good.
- **6 GHz and Wi-Fi 7** on the cards that have them (new, untested on hardware).
- Wi-Fi menu, saved networks, scanning, restart and shutdown.

**Not yet:** AirDrop/AWDL/Continuity, Wi-Fi 7 multi-link, 160/320 MHz
channels. Sleep and wake are untested.

Tested on macOS Sequoia 15 and Tahoe 26.

## Requirements

- A Hackintosh booting with **OpenCore**, with **Lilu**.
- `Misc -> Security -> SecureBootModel` set to `Disabled` (the installer can do this).
- The **old Wi-Fi stack**, which macOS needs for this driver. Download all
  three (each link is a zip with the kext inside):
  - [IOSkywalkFamily.kext](https://github.com/dortania/OpenCore-Legacy-Patcher/raw/d9604c36a432eaf243ea18659ff4d208187452d7/payloads/Kexts/Wifi/IOSkywalkFamily-v1.2.0.zip)
  - [IO80211FamilyLegacy.kext](https://github.com/dortania/OpenCore-Legacy-Patcher/raw/d9604c36a432eaf243ea18659ff4d208187452d7/payloads/Kexts/Wifi/IO80211FamilyLegacy-v1.0.0.zip)
  - [AMFIPass.kext](https://github.com/dortania/OpenCore-Legacy-Patcher/raw/d9604c36a432eaf243ea18659ff4d208187452d7/payloads/Kexts/Acidanthera/AMFIPass-v1.4.1-RELEASE.zip)

  They come from OpenCore Legacy Patcher; they are Apple's and Dhinak G's, so
  this project does not include them.

## Installing

1. **Back up:** copy your OpenCore EFI to a USB stick and check it boots.
2. Download the three kexts above and unzip them into your **Downloads** folder.
3. Download `AirPort_RTW89-<version>.zip` from
   [Releases](https://github.com/FranciumOS-org/Airport_RTW89/releases) and unzip it.
4. Run the **Kext Installer** that comes with it:
   - **macOS:** double-click `Kext Installer.command` (first time: right-click -> Open).
   - **Windows:** double-click `Kext Installer.cmd`.
   - **Linux:** `sh "Kext Installer.sh"`.

   It finds your `config.plist`, shows what it will change, and asks before
   doing anything. Your old config is saved as `config.plist.pre-airport-rtw89`.
   If Python is missing, it offers to install it.
5. Restart. Wi-Fi shows up in the menu bar within about 30 seconds.

To **update**, run the Kext Installer from the new release.

<details>
<summary><b>Installing by hand</b></summary>

1. Copy `AirPort_RTW89.kext` and the three kexts into `EFI/OC/Kexts`.
2. Delete `IO80211FamilyLegacy.kext/Contents/PlugIns/AirPortBrcmNIC.kext`
   (it is for Broadcom cards).
3. In ProperTree, run **OC Snapshot**, then check the order in
   `Kernel -> Add`. It must be: Lilu, IOSkywalkFamily, IO80211FamilyLegacy,
   AMFIPass, AirPort_RTW89, then its plugin
   `AirPort_RTW89.kext/Contents/PlugIns/AirPortRTW89Front.kext`. Move entries
   if Snapshot put them elsewhere.
4. Add to `Kernel -> Block`: `Identifier` `com.apple.iokit.IOSkywalkFamily`,
   `Strategy` `Exclude`, `Arch` `x86_64`, `MinKernel` `23.0.0`, `Enabled` true.
5. Set `Misc -> Security -> SecureBootModel` to `Disabled`.
6. On macOS Tahoe, add `-amfipassbeta` to `boot-args`.
7. If the same EFI also boots Ventura or older, give the four kexts
   `MinKernel` `23.0.0`.

</details>

## Troubleshooting

- **macOS does not start:** boot from your backup USB stick, then remove the
  kexts or restore `config.plist.pre-airport-rtw89`.
- **WPA3 causes trouble:** add the boot-arg `-rtw89nowpa3`.
- **Collect logs for a report:** download the tools zip
  (`AirPort_RTW89-<version>-tools.zip`) and run `sudo tools/collect_logs.sh`.
  It makes one zip with the driver's and macOS's Wi-Fi logs. Passwords are
  never included and MAC addresses are shortened, but network names can
  appear, so check before posting publicly.
- `tools/rtw89ctl status` (from the tools zip) shows the connection, channel and speed.

## Reporting

Open an issue at <https://github.com/FranciumOS-org/Airport_RTW89/issues>
with your card, macOS version and the log zip. [TESTING.md](TESTING.md) lists
what to try.

## Building

On macOS with the Command Line Tools:

```sh
make fetch-firmware   # once: Realtek's firmware, from linux-firmware
make release          # build/release/AirPort_RTW89-<version>.zip and the tools zip
```

The build runs the driver's tests in userspace first (`make hosttest`). To
install a build on this machine: `sudo efi/install.sh` (see
[efi/README.md](efi/README.md)).

| Path | What |
|---|---|
| `src/kext/`, `src/front/` | The kext: PCI device and macOS Wi-Fi interface |
| `src/compat_rtw89/`, `src/compat/` | The Linux parts rtw89 needs, written for macOS (association, WPA2/WPA3, data path) |
| `third_party/` | Linux rtw89, Linux 802.11 headers, hostap and Mbed TLS (for WPA3), unmodified |
| `installer/`, `efi/` | The Kext Installer and the shell installer |
| `tools/` | `rtw89ctl`, the log collector, the tests |

## Licence

GPL-2.0 ([LICENSE](LICENSE)). Vendored code keeps its own licence: Linux rtw89
GPL-2.0 OR BSD-3-Clause, hostap BSD-3-Clause, Mbed TLS Apache-2.0 OR
GPL-2.0-or-later. The kext includes Realtek's firmware, redistributed under
[firmware/LICENCE.rtlwifi_firmware.txt](firmware/LICENCE.rtlwifi_firmware.txt).
See [CREDITS.md](CREDITS.md).
