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

**M0 is done** (2026-10-01): every object compiles and `make link` joins them
into one relocatable object whose only undefined symbols are 50 kernel imports
(listed in [kernel-imports.md](kernel-imports.md)).

**M1 is done** (2026-10-01, macOS 15.8.1, TUF A15). `make kext` produces
`build/out/AirPort_RTW89.kext`: an `IOService` that matches `10EC:B852`/`B85B`,
maps BAR 2, and runs the driver's own `probe()` through the platform glue
(`src/kext/` ↔ `src/compat_rtw89/rtw89_glue.[ch]`). All 349 symbols it imports
resolve against the KPIs it declares (`tools/check_kpi.py`). Firmware
`rtw8852b_fw-2.bin` is embedded.

Loaded twice with `sudo tools/load.sh` and unloaded twice with
`sudo tools/unload.sh`, no panic, same result both times: power-on, firmware
download and efuse read take about 0.35 s, and the kernel log shows

    [rtw89 INFO rtw89_8852be] Firmware version 0.29.29.18 (9e3d777f), cmd version 0, type 5
    [rtw89 INFO rtw89_8852be] chip info CID: 0, CV: 1, AID: 0, ACV: 1, RFE: 1
    AirPort_RTW89: RTL8852BE cut B, RFE type 1, 2T2R
    AirPort_RTW89: MAC address a8:41:f4:e1:b1:4a

Things learned from the real load: Sequoia loads the loose, ad-hoc-signed kext
directly (no approval prompt, no reboot) with this machine's SIP settings; MSI is
available (interrupt index 1); and `log show` does not contain the lines a kext
prints while it is being unloaded, `dmesg` does (which is why `unload.sh` reads
that).

**Towards M2: radio up and scan at the driver level, working on hardware**
(2026-10-01). `sudo build/out/rtw89ctl scan` on the TUF A15 listed 76 networks
in about 2.3 s over 39 channels, 2.4 and 5 GHz including DFS channels heard
passively, and the kext unloaded cleanly afterwards. That proves the hardware
start, MSI interrupts, the RX path through the bouncing DMA ops, firmware
commands and events, and scan offload. Before any IO80211 work, the glue can do what mac80211 does when an
interface is opened and a scan requested: `rtw89_glue_up()` (driver `start()`,
one station interface, interrupts on), `rtw89_glue_scan()` (firmware scan
offload over all enabled channels) and `rtw89_glue_down()`. Received beacons and
probe responses are collected in a table. `build/out/rtw89ctl up|scan|down|status`
drives it through the kext's `setProperties` (administrators only) and prints the
networks. This exercises interrupts, the RX path, firmware commands and scan
offload without needing the legacy Wi-Fi stack or any EFI change.

**Towards M3: association (works on hardware), WPA2 key handshake (built, not
yet run on hardware).**
`src/compat_rtw89/rtw89_mlme.c` is a station MLME for one non-MLO interface,
following `net/mac80211/mlme.c` call for call: channel context on the AP's
channel, station entry, open-system authentication, association (802.11n on
20 MHz, WMM, a WPA2-PSK/CCMP RSN element), `sta_state` transitions,
`vif_cfg_changed`/`link_info_changed`, EDCA parameters, and the reverse on
leaving or when the AP deauthenticates. `rtw89ctl join SSID` / `leave` drive it.
On the test machine it authenticated and associated with a WPA2 access point on
5220 MHz (AID 6, 802.11n, WMM) and received the AP's first handshake messages.

The same file is the WPA2-PSK supplicant: 4-way handshake and group key
handshake (EAPOL-Key descriptor version 2: HMAC-SHA1 MIC, AES key wrap), with
the CCMP keys given to the driver through `set_key`. The cryptography is in
`src/compat_rtw89/rtw89_crypto.c` (SHA-1, HMAC, PBKDF2, the 802.11 PRF, AES-128,
RFC 3394 key wrap), checked against the published vectors in the self-test. It
follows wpa_supplicant where that matters for security: the key derived from an
(unauthenticated) message 1 stays provisional until a MIC verifies with it, a
repeated message 3 does not install the same key again (no packet number reuse),
the SNonce is kept while the AP retries message 1, and the replay counter must
advance. `rtw89ctl join` asks for the password at a prompt; the kext turns it
into the PMK and keeps only that, in memory, until the radio goes down.
Not checked yet: that the RSN element in message 3 equals the one in the beacon
(a downgrade check that matters once more than one cipher/AKM is supported).

Not there yet: the data path (so no network interface and no traffic), receive
packet number checks, wider channels and VHT/HE. Networks that are WPA3-only,
enterprise, WEP/WPA1, or require management frame protection are refused with a
message.

The reference for the IOKit side of M2/M3 is `reference/airport_rtw88/`
(AirPort_RTW88's IO80211 controller and its own MLME, GPL-2.0, not compiled).

Regulatory: with no country known, `wiphy_register()` applies the world domain
(channels 12-14 and all of 5 GHz listen-only, radar flags on 5250-5730 MHz,
nothing above 5835 MHz), so the scan only sends probe requests on 2.4 GHz
channels 1-11. `hw->conf.flags` never has `IEEE80211_CONF_IDLE` yet, so the chip
stays powered between `up` and `down` instead of using rtw89's idle power-off.

Before any load, `make hosttest` runs the same objects in userspace
(`tools/hosttest/`): pthread stand-ins for the 50 kernel imports, then

- a self-test of the compat helpers and the cfg80211/mac80211 stand-in,
- `probe()`/`remove()` twice against a PCI device that never answers (the
  power-on timeout and every error unwind),
- the same with only the hardware steps replaced (`fakechip_core.c`:
  `rtw89_chip_info_setup()` and `rtw89_core_start()`/`stop()`), so the real
  firmware image is parsed, the device is fully registered, the radio is brought
  "up", an interface added, a scan request built, the periodic tracking work run,
  and everything taken down and removed again. In this variant the test also
  plays an access point and WPA2 authenticator: beacon, authentication and
  association responses, the 4-way handshake (verifying the station's MICs with
  its own copy of the keys), a group key renewal, a repeated and a replayed
  message 3, a forged message 1, a wrong password, a refusal, silence (three
  tries then timeout), an AP that never starts the handshake, a
  deauthentication, leaving while connected, and the radio going down while
  connected.

That found and fixed, before they could panic the machine: `pcie_capability_*`
writing to the wrong config-space offset, a failed firmware decompress reported
as success, `dma_free_coherent(NULL)` not accepted, and a NULL dereference in
`rtw89_regd_init_hint()` because registration did not report the initial
regulatory domain the way Linux does. Reading the driver's scan path against the
shims also turned up `skb_copy()` dropping tailroom (a heap overflow when the
driver appends probe-request IEs) and a station interface whose `bss_conf.bssid`
was NULL. All three runs are also clean under Guard
Malloc. What it cannot cover is anything that needs the chip to answer: power-on,
firmware download, efuse.

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
| `src/compat_rtw89/rtw89_glue.[ch]` | Platform boundary, plain C: config space, mapped BAR and DMA memory in; `probe`/`remove`/interrupt/info out. Builds the `pci_dev`, PCI ops and bouncing DMA ops |
| `src/kext/` | The IOKit side (`AirPort_RTW89`), `Info.plist`, kmod descriptor. Sees only `rtw89_glue.h` |

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

Cosmetic: the kernel's `printf` does not know Linux's `%ph`/`%pM` extensions, so
a few driver messages print a pointer where Linux prints bytes ("Firmware element
BB version: 0x...h"). The kext logs the MAC address itself.

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
| M1 ✅ | **Talks to the chip** — standalone IOService (no IO80211 yet): match `10EC:B852`, map BAR, `rtw89_pci_probe` → power on → firmware download → read efuse | `dmesg` shows firmware version and the card's real MAC address; kext unloads cleanly |
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
- `tools/hosttest/` — `make hosttest`, see Status.
- `tools/check_kpi.py` — `make kext`: every import of the built kext must come from a declared KPI.
- `tools/load.sh`, `tools/unload.sh` — run by a person with sudo, never automatically.
- `tools/rtw89ctl.c` — built by `make kext` as `build/out/rtw89ctl`: `up`, `scan`, `down`, `status` for a loaded kext.
