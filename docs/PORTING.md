# AirPort_RTW89 — porting Linux rtw89 (RTL8852BE) to macOS

Goal: a native-AirPort kext for the Realtek RTL8852BE (`10EC:B852`, Wi-Fi 6, PCIe),
built the same way as AirPort_RTW88 in
[Realtek-AirPort-Family](https://github.com/xnoah222/Realtek-AirPort-Family):
the unmodified Linux driver as the hardware backend, a Linux-API compat layer
underneath it, and IO80211 (legacy stack) glue on top.

## Why not just add the PCI ID to AirPort_RTW88

The 8852BE is not an rtw88 chip. Linux drives it with `rtw89`, a separate driver
with its own firmware protocol, MAC generation (`mac_ax`), PHY/RF calibration, and
BT-coexistence engine. Adding `B852` to the rtw88 personality would at best fail to
probe and at worst panic.

## Source baseline

| Input | Pin |
|---|---|
| Linux rtw89 | `torvalds/linux` @ `551c722` (2026-09-29), `drivers/net/wireless/realtek/rtw89` — GPL-2.0 OR BSD-3-Clause |
| Compat + macOS glue | Realtek-AirPort-Family @ `1885e37`, `AirPort_RTW88/Feixiao` — GPL-2.0 |
| Firmware | linux-firmware `rtw89/rtw8852b_fw-1.bin` (pin a commit when vendored) |

## Status

**M0 is done** (2026-10-01): all 35 objects compile and `make link` joins them
into one relocatable object whose only undefined symbols are 50 kernel imports
(listed in [kernel-imports.md](kernel-imports.md)). There is no kext bundle yet
and nothing has been loaded; that starts with M1.

How M0 got there differs from the original plan below in one important way:
instead of extending the simplified mac80211 shim inherited from AirPort_RTW88,
rtw89 is built against the **unmodified upstream** `linux/ieee80211*.h`,
`nl80211.h`, `ieee80211_radiotap.h`, `net/cfg80211.h` and `net/mac80211.h`
(`third_party/linux-include/`, same commit as the driver). The compat layer
supplies the kernel infrastructure underneath those headers, and two new files
supply the code behind them:

| File | What |
|---|---|
| `src/compat_rtw89/rtw89_compat.h` | Force-included. Kernel API additions (scope guards, kconfig, bitfield/bitmap, list, skb, PCI helpers), then includes the upstream `net/mac80211.h` |
| `src/compat_rtw89/rtw89_cfg80211.c` | wiphy allocation, `wiphy_work`, regulatory hint echo, frame/channel helpers |
| `src/compat_rtw89/rtw89_cfg80211_bitrate.c` | `cfg80211_calculate_bitrate()`, copied verbatim from `net/wireless/util.c` |
| `src/compat_rtw89/rtw89_mac80211.c` | hw allocation, vif/station/key lists behind the iterators, TXQ queues and scheduling, queue stop/wake, null-func/PS-Poll/probe-request templates, emulated chanctx |
| `src/compat_rtw89/rtw89_net80211.h` | Internal state of the two files above and the interface the IOKit layer will use (`struct rtw89_m80211_glue_ops`, vif/sta/key lifetime, `rtw89_m80211_tx`) |

The first compile against the inherited shim had 3,664 errors; the header switch
alone took that to 235 because every 802.11 constant and struct is now the real
one. The struct-coverage notes in "Size of the job" are kept as history.

Present but deliberately not functional yet (all in `rtw89_mac80211.c`, each
commented where it is defined):

- **Upcalls** (RX frames, TX status, scan done, link loss, queue state, BlockAck
  requests) go to `rtw89_m80211_glue_ops`. Nothing registers those hooks until the
  IOKit layer exists, so frames are freed and events dropped.
- **`ieee80211_restart_hw`** (firmware error recovery, SER level 2) only logs. M4.
- **AP mode, P2P remain-on-channel, MLO**: return NULL / error. Out of scope.
- **`cfg80211_bss_iter`** walks nothing: scan results live in IO80211.
- **Firmware**: the blob table is empty until `make fetch-firmware` is run, so
  probe would fail with "firmware not in embedded blobs".

Known limitation inherited from the rtw88 compat layer: `spinlock_t` is a
heap-allocated `IORecursiveLock` (a sleeping lock) and Linux code never frees
spinlocks, so each `spin_lock_init` in driver code leaks one lock object when
its owner is freed. That is a fixed number per device, lost at unload.

## Size of the job

8852BE subset of rtw89 (rtw89_core + rtw89_pci + 8852b chip modules):
**~108k lines of C** (≈23k of which are register/RF tables) + ~36k lines of headers.

API gap against the AirPort_RTW88 compat layer (`tools/api_gap.py`, heuristic):
**393** external identifiers referenced, **263** already shimmed, **130** missing.
Full list: [api-gap.md](api-gap.md). Summary:

| Area | Missing | Notes |
|---|---|---|
| mac80211 | 38 | `wiphy_work`/`wiphy_delayed_work` (47 refs) is the big one; the rest are small helpers or MLO no-ops |
| bit/field helpers | ~50 | `u32_replace_bits`, `le16_encode_bits`, `FIELD_GET_SIGNED`, bitmap ops — mechanical |
| ACPI/DMI | 8 | stub: no SAR/regulatory policy from ACPI initially |
| cfg80211 | 5 | chandef/IE helpers |
| skb/sync/time/mem | ~20 | mostly one-liners over existing shims |

Struct coverage: the shim already defines the link-era mac80211 structs rtw89
uses (`ieee80211_vif`, `bss_conf`, `link_sta`, `chanctx_conf`, `txq`, `key_conf`).
Missing: `ieee80211_sta_he_cap`, `ieee80211_sta_eht_cap`,
`ieee80211_sband_iftype_data`, `wiphy_work`, plus HE/EHT fields read from
`link_sta` / `bss_conf` (`he_cap`, `eht_cap`, `he_bss_color`, `he_oper`, `tpe`,
`power_type`, `chanreq`). The regex tool does not check struct *fields*; the first
compile will.

## What gets compiled vs. stubbed

| File | Plan |
|---|---|
| core, mac, mac_be, phy, phy_be, fw, cam, efuse, efuse_be, chan, ser, ps, sar, util, pci, pci_be | compile as-is (`mac_be`/`phy_be`/`pci_be`/`efuse_be` are referenced by generic tables even though 8852B is an AX chip) |
| rtw8852b, rtw8852b_common, rtw8852b_table, rtw8852b_rfk, rtw8852b_rfk_table, rtw8852be | compile as-is |
| coex | compile as-is — the chip needs coex init even with BT unused |
| mac80211.c | compile; it becomes the ops table the macOS glue calls (like rtw88's `mac80211.c`) |
| regd | compile; shim returns a fixed world regdomain |
| acpi | stub (no ACPI policy) |
| wow | excluded (`CONFIG_PM` off) until sleep/wake milestone |
| debug | `debug.c` (debugfs) excluded; built with `CONFIG_RTW89_DEBUGMSG` and `src/compat_rtw89/rtw89_debug_shim.c` routes `rtw89_debug` to `IOLog` behind the `rtw89_debug=` boot-arg |

Upstream files stay byte-identical; every macOS difference lives in the compat
layer or in `#ifdef RTW89_MACOS` patches kept as a numbered patch series so rebasing
onto newer Linux is mechanical.

## Milestones

Each milestone ends with something observable on real hardware.

| # | Milestone | Done when |
|---|---|---|
| M0 ✅ | **Builds** — fork Feixiao glue into this repo, vendor the rtw89 subset + firmware, extend shims until everything compiles and links | `make link`: zero undefined symbols except kernel imports. (The kext bundle itself needs the IOKit glue and is the first step of M1.) |
| M1 | **Talks to the chip** — standalone IOService (no IO80211 yet): match `10EC:B852`, map BAR, `rtw89_pci_probe` → power on → firmware download → read efuse | `dmesg` shows firmware version and the card's real MAC address; kext unloads cleanly |
| M2 | **Scans** — attach to IO80211FamilyLegacy using the adapted `RTW88IEEE80211` layer; rtw89 uses firmware `hw_scan` | Networks appear in the Wi-Fi menu |
| M3 | **Associates** — open + WPA2 via the existing internal RSN/EAPOL path | DHCP lease, pings |
| M4 | **Stable** — sustained traffic, network switching, SER (firmware error recovery), sleep/wake | 1 h iperf without drops; survives 10 sleep cycles |
| M5 | **Ships** — PR upstream to Realtek-AirPort-Family or standalone release | Other 8852BE owners confirm |

HE (802.11ax) rates are reported where IO80211 accepts them; otherwise the
driver still runs HE on-air and reports VHT to macOS. AWDL is out of scope.

## Environments

Everything happens on one machine since 2026-10-01: the ASUS TUF A15 (FA507NU:
Ryzen 7 7735HS, RTL8852BE, RTL8168 GbE) booted into macOS. Editing, building
(`make`, Command Line Tools are enough, no full Xcode needed for the C objects)
and hardware testing all run there. The Windows + VMware build VM used before
that is no longer part of the loop.

### TUF A15 as the test machine

The card stays in place, but the laptop has to become a (minimal) hackintosh
first. Only what driver testing needs matters here:

- **CPU**: Zen 3+ (family 19h) → AMD_Vanilla kernel patches with the core count set to 8.
- **Graphics**: none accelerated. The Radeon 680M (RDNA2 iGPU) has no macOS driver
  (NootedRed only covers Vega APUs) and the RTX 4050 has none either. The internal
  panel runs on the firmware (GOP) framebuffer — slow UI, fine for Terminal + logs.
- **Network while Wi-Fi is broken**: RTL8168 → `RealtekRTL8111.kext`. Needed for
  copying kext builds over and for `log stream` over SSH from Windows.
- **Input**: ASUS I2C touchpad is the risky part on AMD; a USB mouse/keyboard
  removes it from the critical path.
- **Target OS**: Tahoe 26, with the IOSkywalkFamily downgrade + AMFIPass the
  AirPort_RTW88 README already requires on Sonoma+. The machine currently runs
  Sequoia 15.8.1, which is under the same Sonoma+ requirement.

The EFI is its own sub-project and has to boot to a desktop before M1.

Test hygiene: keep a known-good EFI on a USB stick; load the dev kext with
`kextutil`/`kmutil load` from a running system rather than from OpenCore until M2,
so a panic costs one reboot, not a recovery session. Add `keepsyms=1 debug=0x100`
to boot-args so panics show symbolized backtraces.

## Tools

- `tools/api_gap.py` — regenerate [api-gap.md](api-gap.md) after upstream bumps or shim changes (regex estimate; the compiler and `make link` are the ground truth).
- `tools/errsum.py` — `make errors`: group compiler errors into [compile-status.md](compile-status.md).
- `tools/check_imports.py` — `make link`: list what is left for the kernel in [kernel-imports.md](kernel-imports.md) and fail on anything the kernel does not have.
- `tools/fetch_linux.sh` — re-download `third_party/linux-include` and `third_party/linux-reference` at the pinned commit.
- `tools/extract_bitrate.sh` — rebuild `rtw89_cfg80211_bitrate.c` from the fetched `net/wireless/util.c`.
- `tools/gen_fw_blobs.py` — embed `firmware/*.bin` (run by the Makefile).
