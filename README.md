# AirPort_RTW89

Native Wi-Fi on macOS for the **Realtek RTL8852BE** (PCI `10EC:B852`, Wi-Fi 6),
by porting the Linux **rtw89** driver. The card shows up as Wi-Fi in macOS and
is used from the Wi-Fi menu and System Settings like a Mac's own.

> **Preview (0.1.0).** It has been developed and tested on **one machine**
> (ASUS TUF A15 FA507NU, macOS Sequoia 15.8.1). It works there every day, but
> nobody else has run it yet. A driver bug can panic the machine: keep a way
> to boot without it (see [Installing](#installing)). Reports from other
> RTL8852BE owners are what this preview is for.

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
- **2.4 GHz uploads are slow** (about 11 Mb/s measured, against 66 Mb/s on 5 GHz
  on the same router): use 5 GHz where you can.
- **6 GHz**: the RTL8852BE has none.
- Only **Sequoia 15.8.1** is tested. Sonoma and Tahoe are expected to work
  (the Tahoe join request is handled) but are untested.

## Requirements

- A Hackintosh booting with **OpenCore**, with `Lilu.kext`.
- `Misc -> Security -> SecureBootModel` set to `Disabled`.
- The **old Wi-Fi stack**, which OpenCore loads in place of the one macOS ships
  (Sonoma and later dropped what this driver needs): `IOSkywalkFamily.kext`
  (Ventura's), `IO80211FamilyLegacy.kext` and `AMFIPass.kext`. These are
  Apple's and the OpenCore Legacy Patcher project's, not part of this project;
  [efi/README.md](efi/README.md) says where to download them. No root patch of
  the system volume is needed.
- The development machine runs with `csr-active-config` `0xA03`; full SIP has
  not been tried.

## Installing

1. **Keep a way back.** Copy your current OpenCore EFI to a USB stick and make
   sure you can boot from it. If macOS ever fails to start after installing,
   boot from the stick.
2. Download the three kexts of the old Wi-Fi stack and unpack them into
   `efi/kit/` ([efi/README.md](efi/README.md)).
3. From the folder of this release:
   ```sh
   sudo efi/install.sh
   ```
   It finds your OpenCore EFI (if there are several it asks which), saves your
   config as `config.plist.pre-airport-rtw89`, copies the kexts and adds their
   entries. Nothing else in your config is changed.
4. Restart. Wi-Fi appears in the menu bar within about 30 seconds of the
   desktop.

`sudo efi/uninstall.sh` switches it all off again.

## When something goes wrong

```sh
sudo tools/collect_logs.sh
```

saves the driver's own log and macOS's Wi-Fi log of this boot into `build/log/`.
Neither contains passwords. Attach them to a report, with your machine, macOS
version and what you did.

`rtw89ctl status` (no sudo) shows the link: network, channel, rates, and what
the driver hears around it. `sudo rtw89ctl log` prints the driver's log.

## Building from source

On macOS with the Command Line Tools:

```sh
make fetch-firmware   # once: Realtek's firmware from linux-firmware
make kext front       # build/out/AirPort_RTW89.kext and AirPortRTW89Front.kext
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
| `third_party/rtw89/` | Linux rtw89 for the RTL8852BE, **unmodified** |
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
