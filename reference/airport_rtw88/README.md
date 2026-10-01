# AirPort_RTW88

AirPort_RTW88 is a native macOS AirPort Wi-Fi driver for Realtek RTL88xx PCIe wireless adapters.

It ports the Linux `rtw88` driver to macOS and integrates it with Apple's native AirPort/IO80211 Wi-Fi stack, allowing supported Realtek adapters to appear and operate through the built-in macOS Wi-Fi interface.

AirPort_RTW88 uses the Linux `rtw88` driver as its hardware backend, with macOS integration inspired by and adapted from OpenIntelWireless `itlwm` / `AirportItlwm`.

## Current Status

AirPort_RTW88 1.0.1 is a maintenance and hardware-compatibility release built on the working native AirPort path from 1.0.0.

Currently working:

- Native AirPort/IO80211 interface
- Wi-Fi network scanning, including fixes for overlapping CoreWiFi scans
- 2.4 GHz networks
- 5 GHz networks
- Association and connection through the native macOS Wi-Fi interface
- Open networks
- WPA/WPA2 networks
- WPA/WPA2 association using the driver's internal RSN/EAPOL path
- Normal IP network traffic after association
- Expanded PCIe device-ID coverage for supported RTL88xx families

### Experimental development code

The tree also contains ongoing AWDL/P2P work used for AirDrop/Continuity research. It is **not a supported feature of the 1.0.1 release** and should be considered incomplete.

### Not Currently Supported

- Reliable/full AWDL channel scheduling and availability windows
- Guaranteed AirDrop/Handoff/Universal Clipboard interoperability
- USB Realtek Wi-Fi adapters (intentionally excluded from the build)
- SDIO Realtek Wi-Fi adapters (intentionally excluded from the build)

## Supported Hardware

### Tested Hardware

- **RTL8822BE** — tested and confirmed working

### PCIe Family Coverage

The AirPort target now matches only PCIe devices. The following RTL88xx PCIe chipsets have a device-ID → `rtw_chip_info` mapping in `RTW88IEEE80211.cpp`, an IOKit PCI personality, and their required firmware blobs embedded in AirPort_RTW88:

- **RTL8822BE** — `10EC:B822` (tested)
- **RTL8822CE** — `10EC:C822`, `10EC:C82F`
- **RTL8821CE** — `10EC:C821`, `10EC:B821`

Only RTL8822BE has been physically validated so far. The other PCIe IDs are enabled for field testing because the project contains their chip core/hw-spec mapping and firmware. They must remain marked **experimental/unverified** until tested on real hardware.

> **PCIe only:** USB and SDIO transports/frontends are intentionally not compiled into the AirPort target. Upstream source files may remain in the vendored `rtw88-stable` tree, but they are not part of the built kext.

## Building

AirPort_RTW88 requires:

- Xcode Command Line Tools
- MacKernelSDK
- Linux `rtw88` source

Clone the repository and its submodules:

```sh
git clone --recursive https://github.com/xnoah222/Airport_RTW88.git
cd Airport_RTW88
```

If the repository was cloned without `--recursive`:

```sh
git submodule update --init --recursive
```

AirPort_RTW88 currently expects the Linux `rtw88` source tree to be located next to the repository.

Clone `rtw88-stable`:

```sh
cd ..
git clone https://github.com/thegwchr/rtw88-stable.git
```

The directory structure should look like:

```text
parent/
├── Airport_RTW88/
└── rtw88-stable/
```

The Linux driver source should therefore be available at:

```text
../rtw88-stable/drivers/net/wireless/realtek/rtw88/
```

If you use a different location, update `LINUX_SRC` in the `Makefile`.

Build AirPort_RTW88 with:

```sh
make airport
```

The resulting kernel extension is:

```text
build/out/AirPort_RTW88.kext
```

## Installation

AirPort_RTW88 is intended primarily for use through OpenCore.

Add:

```text
AirPort_RTW88.kext
```

to your OpenCore `EFI/OC/Kexts` directory and add the corresponding entry to `Kernel -> Add` in `config.plist`.

After rebooting, the Realtek adapter should be exposed to macOS through the native AirPort Wi-Fi interface if initialization succeeds.

For development and debugging, the kext may also be loaded using the appropriate macOS kernel-extension development tools depending on the macOS version and system configuration.

## Troubleshooting

Driver messages can be inspected with:

```sh
log show --last boot --predicate 'process == "kernel"' | grep -Ei 'rtw88|AirportRTW88'
```

For live debugging:

```sh
log stream --predicate 'process == "kernel"' --info | grep -Ei 'rtw88|AirportRTW88'
```

Useful messages include firmware loading, PCI initialization, scanning, association, EAPOL/RSN state and network-interface initialization.

If the adapter is not detected, verify that:

- AirPort_RTW88.kext is actually loaded
- The PCI device is visible to macOS
- The card uses a supported RTL88xx PCIe chipset
- The required firmware is available
- OpenCore is injecting the kext correctly

## Project Structure

AirPort_RTW88 consists of three main layers:

- The ported Linux `rtw88` hardware/PHY/MAC implementation
- The macOS AirPort/IO80211 STA integration layer
- The internal `RTW88AWDLManager` + virtual-interface layer used for AWDL/P2P bring-up

The macOS layer exposes the Realtek hardware as a native Wi-Fi interface instead of presenting it as an Ethernet adapter.

Some internal class names retain the `AirportRTW88` naming used during development.

## Known Limitations

AirPort_RTW88 1.0.1 is an experimental feature release built on the working 1.0.0 STA path.

AWDL/P2P and AirDrop support are partial. The driver now contains the IO80211 virtual-interface bridge, verified AWDL control payloads and native management/action-frame transport, but off-channel scheduling and full interoperability still require hardware testing and follow-up fixes.

Support for RTL88xx models other than the RTL8822BE is currently unverified due to lack of physical hardware testing.

## Acknowledgements

AirPort_RTW88 would not be possible without work from several open-source projects and communities:

- **Linux rtw88 contributors** — for the original Realtek RTL88xx driver implementation
- **OpenIntelWireless** — for `itlwm` and `AirportItlwm`, used as important references for macOS Wi-Fi and AirPort integration
- **Acidanthera** — for MacKernelSDK
- **Apple** — for macOS, IO80211 and the surrounding networking frameworks
- **FreeBSD** — for LinuxKPI and its work adapting Linux driver concepts to BSD environments

See `CREDITS.md` and the source files for additional attribution and licensing information.

## Disclaimer

AirPort_RTW88 is an independent open-source project and is not affiliated with or endorsed by Realtek, Apple, OpenIntelWireless, Acidanthera, or the Linux kernel project.

Use at your own risk. Kernel extensions operate with high privileges and may cause system instability or kernel panics, particularly on unsupported hardware.

## License

AirPort_RTW88 contains and adapts code originating from multiple open-source projects.

Licensing and copyright notices present in individual source files must be preserved. See `CREDITS.md` and the relevant source files for attribution and licensing details.
