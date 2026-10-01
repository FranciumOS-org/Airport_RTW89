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
| debug | excluded (`CONFIG_RTW89_DEBUGMSG` off); route `rtw89_debug` to `IOLog` behind a boot-arg |

Upstream files stay byte-identical; every macOS difference lives in the compat
layer or in `#ifdef RTW89_MACOS` patches kept as a numbered patch series so rebasing
onto newer Linux is mechanical.

## Milestones

Each milestone ends with something observable on real hardware.

| # | Milestone | Done when |
|---|---|---|
| M0 | **Builds** — fork Feixiao glue into this repo, vendor the rtw89 subset + firmware, extend shims until everything compiles and links | `AirPort_RTW89.kext` links with zero undefined symbols (except kernel imports) |
| M1 | **Talks to the chip** — standalone IOService (no IO80211 yet): match `10EC:B852`, map BAR, `rtw89_pci_probe` → power on → firmware download → read efuse | `dmesg` shows firmware version and the card's real MAC address; kext unloads cleanly |
| M2 | **Scans** — attach to IO80211FamilyLegacy using the adapted `RTW88IEEE80211` layer; rtw89 uses firmware `hw_scan` | Networks appear in the Wi-Fi menu |
| M3 | **Associates** — open + WPA2 via the existing internal RSN/EAPOL path | DHCP lease, pings |
| M4 | **Stable** — sustained traffic, network switching, SER (firmware error recovery), sleep/wake | 1 h iperf without drops; survives 10 sleep cycles |
| M5 | **Ships** — PR upstream to Realtek-AirPort-Family or standalone release | Other 8852BE owners confirm |

HE (802.11ax) rates are reported where IO80211 accepts them; otherwise the
driver still runs HE on-air and reports VHT to macOS. AWDL is out of scope.

## Environments

| Role | Machine |
|---|---|
| Editing, analysis | Windows host (ASUS TUF A15, hosts the VMs) |
| Build | macOS VMware VM (Xcode, `make`) — VMware Workstation cannot pass through PCIe, so **no hardware testing in the VM** |
| Hardware test | the ASUS TUF A15 itself (FA507NU: Ryzen 7 7735HS, RTL8852BE, RTL8168 GbE), booted into macOS |

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
  AirPort_RTW88 README already requires on Sonoma+.

The EFI is its own sub-project and has to boot to a desktop before M1.

Test hygiene: keep a known-good EFI on a USB stick; load the dev kext with
`kextutil`/`kmutil load` from a running system rather than from OpenCore until M2,
so a panic costs one reboot, not a recovery session. Add `keepsyms=1 debug=0x100`
to boot-args so panics show symbolized backtraces.

## Tools

- `tools/api_gap.py` — regenerate [api-gap.md](api-gap.md) after upstream bumps or shim changes.
