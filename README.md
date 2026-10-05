# AirPort_RTW89

Native Wi-Fi on macOS for **Realtek's rtw89 PCIe cards**, by porting the Linux
**rtw89** driver. The card shows up as Wi-Fi in macOS and is used from the
Wi-Fi menu and System Settings like a Mac's own.

> **Preview (0.2.1).** It has been developed and tested on **one machine**
> (ASUS TUF A15 FA507NU with an RTL8852BE, macOS Sequoia 15.8.1). It works
> there every day; on every other card it has **never run on real hardware**.
> A driver bug can panic the machine: keep a way to boot without it (see
> [Installing](#installing)). Reports from other machines and cards are what
> this preview is for: see [TESTING.md](TESTING.md).

## Cards

| Card | PCI ID (`10EC:`) | Wi-Fi | State |
|---|---|---|---|
| RTL8852BE | `B852`, `B85B` | 6, 2×2 | **works** (the development machine) |
| RTL8852BTE | `B520` | 6, 2×2 | built in, untested |
| RTL8851BE | `B851` | 6, 1×1 | built in, untested |
| RTL8852AE | `8852`, `A85A` | 6, 2×2 | boots (one tester, Tahoe); joining panicked in 0.2.0, fixed in 0.2.1 |
| RTL8852CE | `C852` | 6E, 2×2, 160 MHz | built in, untested; 6 GHz off (needs WPA3) |
| RTL8922AE | `8922`, `892B` | 7, 2×2 | built in, untested; joins as Wi-Fi 6, 6 GHz off |
| RTL8922DE | `892D`, `882D`, `895D` | 7, 2×2 | built in, untested; joins as Wi-Fi 6, 6 GHz off |

All of them run the same Linux code, unmodified, with each chip's own Realtek
firmware. To see which card you have before installing anything, run
`sudo tools/collect_logs.sh` from this folder: `logs/system.txt` lists the
Realtek PCI devices, the Wi-Fi card as `WLAN` or similar with `device-id=b852`
(or another ID from the table). USB versions of these cards are not supported.

## What works

| | |
|---|---|
| Networks | WPA2-Personal, WPA2-Enterprise (802.1X, tested with PEAP), open; WPA2/WPA3 mixed networks (joined with WPA2) |
| Speed | 802.11ax (Wi-Fi 6), up to 80 MHz, 2 streams: about 500 Mb/s down measured on a 5 GHz 80 MHz network |
| macOS | Wi-Fi menu, joining and forgetting networks, saved passwords, Personal Hotspot from an iPhone, scanning while connected |
| Startup | Loaded by OpenCore at boot, Wi-Fi up without anything run by hand |
| Restart and shutdown | The chip is powered down cleanly |

## What does not (yet)

- **WPA3-only networks** (SAE). Mixed WPA2/WPA3 networks work over WPA2.
  On a phone hotspot set to WPA3 only, choose WPA2/WPA3 instead.
- **Sleep and wake**: the driver handles it, but it has not been tested on
  hardware (the development machine does not really sleep).
- **AirDrop, AWDL, Continuity** over Wi-Fi.
- **Range is still short of Windows**: two rooms from the router, 90 Mb/s on
  5 GHz (peaks of 180) and 50 Mb/s on 2.4 GHz, where Windows gets about 200.
  Transmit power follows the country the router announces; a router that
  announces none leaves the driver on Realtek's cautious worldwide limits
  (`rtw89ctl status` shows which: "tables of 00" is worldwide).
- **6 GHz** is switched off (RTL8852CE, RTL8922AE/DE): every 6 GHz network
  needs WPA3. **Wi-Fi 7** cards join as Wi-Fi 6.
- Only **Sequoia 15.8.1** is tested. Sonoma and Tahoe are expected to work
  (the Tahoe join request is handled) but are untested.

## Requirements

- A Hackintosh booting with **OpenCore**, with `Lilu.kext`.
- `Misc -> Security -> SecureBootModel` set to `Disabled`.
- The **old Wi-Fi stack**, which OpenCore loads in place of the one macOS ships
  (Sonoma and later dropped what this driver needs). Download all three from
  OpenCore Legacy Patcher, where they are published (they are Apple's and
  Dhinak G's, so this project does not include them):
  - [IOSkywalkFamily.kext](https://github.com/dortania/OpenCore-Legacy-Patcher/raw/d9604c36a432eaf243ea18659ff4d208187452d7/payloads/Kexts/Wifi/IOSkywalkFamily-v1.2.0.zip) (Ventura's, v1.2.0)
  - [IO80211FamilyLegacy.kext](https://github.com/dortania/OpenCore-Legacy-Patcher/raw/d9604c36a432eaf243ea18659ff4d208187452d7/payloads/Kexts/Wifi/IO80211FamilyLegacy-v1.0.0.zip) (v1.0.0)
  - [AMFIPass.kext](https://github.com/dortania/OpenCore-Legacy-Patcher/raw/d9604c36a432eaf243ea18659ff4d208187452d7/payloads/Kexts/Acidanthera/AMFIPass-v1.4.1-RELEASE.zip) (v1.4.1)

  Each link downloads a zip with the kext inside. No root patch of the system
  volume is needed.
- The development machine runs with `csr-active-config` `0xA03`; full SIP has
  not been tried.
- Python 3, which the installer uses to edit `config.plist`: Apple's Command
  Line Tools have it (`xcode-select --install`).

## Installing

1. **Keep a way back.** Copy your current OpenCore EFI to a USB stick and make
   sure you can boot from it. If macOS ever fails to start after installing,
   boot from the stick.
2. Download the three kexts of the old Wi-Fi stack from the links in
   [Requirements](#requirements) and unzip them.
3. Put them in, one way or the other:
   - **With the Kext Installer (easiest)**, which comes with the kext in
     `AirPort_RTW89-<version>.zip`, on macOS, Windows or Linux. Unzip it, leave
     the three downloaded kexts unzipped in your Downloads folder (or next to
     the installer), then run:
     - macOS: double-click `Kext Installer.command` (the first time,
       right-click it -> Open, as it comes from the internet);
     - Windows: double-click `Kext Installer.cmd` (it asks for administrator
       rights: the EFI partition needs them);
     - Linux: `sh "Kext Installer.sh"`.

     It asks for your `config.plist` (drag it into the window, or press Enter
     and it looks for, and offers to mount, the EFI partition), shows what it
     will do and asks before doing it: copies the kexts, leaves out the
     Broadcom plugin, saves your config as `config.plist.pre-airport-rtw89`,
     puts the entries in the order they must load in, adds the Block entry,
     and offers to set SecureBootModel and, for Tahoe, `-amfipassbeta`.
     Nothing else in your config is changed. It needs Python 3 and installs
     it if it is missing, after asking: Apple's Command Line Tools on macOS,
     Python 3.12 (winget, else python.org) on Windows, the package manager's
     on Linux.
   - **By hand**, as with any kext:
     1. Download `AirPort_RTW89.kext` (the release's
        `AirPort_RTW89-<version>.zip`); you have the other three from step 2.
     2. Copy the four `.kext`s into `EFI/OC/Kexts`. Delete
        `IO80211FamilyLegacy.kext/Contents/PlugIns/AirPortBrcmNIC.kext`
        (it is for Broadcom cards).
     3. In ProperTree, **OC Snapshot**, then **check the order** of
        `Kernel -> Add`: after Lilu, `IOSkywalkFamily`,
        `IO80211FamilyLegacy`, `AMFIPass`, `AirPort_RTW89` and its plugin
        `AirPort_RTW89.kext/Contents/PlugIns/AirPortRTW89Front.kext`. OC
        Snapshot can put `AirPort_RTW89` before `IO80211FamilyLegacy`: if so,
        drag it and its plugin below `AMFIPass`. In the wrong order
        OpenCore cannot load the front ("Prelinked injection ...
        AirPortRTW89Front.kext - Invalid Parameter" in its log).
     4. Add one entry to `Kernel -> Block` by hand (Snapshot does not):
        `Identifier` `com.apple.iokit.IOSkywalkFamily`, `Strategy` `Exclude`,
        `Arch` `x86_64`, `MinKernel` `23.0.0`, `Enabled` true. It keeps
        macOS's own IOSkywalkFamily out so that Ventura's takes its place.
     5. If the same EFI also boots Ventura or older, give the four kexts
        `MinKernel` `23.0.0` too.
     6. `Misc -> Security -> SecureBootModel` must be `Disabled`.
   - **With the shell installer** (macOS, what the Kext Installer does
     without the questions), from the tools zip: put the three kexts in its `efi/kit/` and
     `AirPort_RTW89.kext` in the folder itself, then
     ```sh
     sudo efi/install.sh
     ```
     It finds your OpenCore EFI (if there are several it asks which), checks
     there is room, saves your config as `config.plist.pre-airport-rtw89`,
     copies the kexts and adds the entries above. Nothing else in your config
     is changed. `sudo efi/uninstall.sh` switches it all off again.
4. Restart. Wi-Fi appears in the menu bar within about 30 seconds of the
   desktop.

## When something goes wrong

```sh
sudo tools/collect_logs.sh
```

(from the tools zip) saves your machine, macOS version, card, the driver's log and macOS's Wi-Fi
log of this boot into `logs/`, and packs them into one zip to attach to a
report (with what you did). There are no passwords in it, and MAC addresses
are cut to their first half; network names can appear, so look before posting
it publicly. [TESTING.md](TESTING.md) says what to try and send.

`tools/rtw89ctl status` (no sudo) shows the link: network, channel, rates,
the TX power tables, and what the driver hears around it. `sudo tools/rtw89ctl log`
prints the driver's log.

## Reporting

Issues and test reports: <https://github.com/FranciumOS-org/Airport_RTW89/issues>. The source, including everything
the release is built from, is at <https://github.com/FranciumOS-org/Airport_RTW89>.

## Building from source

On macOS with the Command Line Tools:

```sh
make fetch-firmware   # once: Realtek's firmware for each chip, from linux-firmware
make kext front       # build/out/AirPort_RTW89.kext and AirPortRTW89Front.kext
make bundle           # the two as one: build/out/bundle/AirPort_RTW89.kext
make release          # the downloads: build/release/AirPort_RTW89-<version>.zip
                      # (the kext) and AirPort_RTW89-<version>-tools.zip
```

`make kext` first runs `make hosttest`: the same driver code in userspace
against a pretend chip, which has to pass. See [docs/PORTING.md](docs/PORTING.md)
for how the port is put together and [efi/README.md](efi/README.md) for the
development loop.

## How it is put together

| Path | What |
|---|---|
| `src/kext/` | The driver proper: PCI, interrupts, DMA, and the answers to macOS's Wi-Fi requests |
| `src/front/` | The front: the IO80211 controller macOS talks to, loaded by OpenCore with the old Wi-Fi stack |
| `src/compat_rtw89/` | What Linux's rtw89 needs from Linux, written for macOS: the 802.11 association and key handshakes (MLME), the data path, cfg80211/mac80211 |
| `src/compat/` | Linux-API shims inherited from AirPort_RTW88 |
| `third_party/rtw89/` | Linux rtw89: the core and the PCIe chips, **unmodified** |
| `third_party/linux-include/` | Linux 802.11 headers, unmodified |
| `third_party/hostap/`, `third_party/mbedtls/` | For WPA3, not in use yet; unmodified |
| `efi/` | Installing into OpenCore |
| `tools/` | `rtw89ctl`, log collection, the userspace tests |

## Licence

GPL-2.0 ([LICENSE](LICENSE)). The vendored code keeps its own licence:
Linux rtw89 is GPL-2.0 OR BSD-3-Clause, hostap BSD-3-Clause, Mbed TLS
Apache-2.0 OR GPL-2.0-or-later (used under GPL-2.0). The kext contains
Realtek's firmware for the chip, which Realtek allows to be redistributed
under the terms in [firmware/LICENCE.rtlwifi_firmware.txt](firmware/LICENCE.rtlwifi_firmware.txt).
See [CREDITS.md](CREDITS.md).
