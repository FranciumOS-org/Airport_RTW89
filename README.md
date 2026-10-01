# AirPort_RTW89

Work-in-progress port of the Linux **rtw89** driver to macOS for the
**Realtek RTL8852BE** (`10EC:B852`, Wi-Fi 6 PCIe), following the approach of
AirPort_RTW88 in [Realtek-AirPort-Family](https://github.com/xnoah222/Realtek-AirPort-Family).

**Status: M0 done; the M1 kext (probe the chip, download firmware, read the MAC)
builds and passes its userspace smoke test but has not been loaded on hardware yet.
Do not put this in an EFI.**

See [docs/PORTING.md](docs/PORTING.md) for the plan and milestones.

## Layout

| Path | What |
|---|---|
| `third_party/rtw89/` | Vendored Linux rtw89 subset, **unmodified** ([UPSTREAM.md](third_party/rtw89/UPSTREAM.md)) |
| `third_party/linux-include/` | Linux 802.11 headers (`ieee80211*.h`, `nl80211.h`, `cfg80211.h`, `mac80211.h`, …), **unmodified** ([UPSTREAM.md](third_party/linux-include/UPSTREAM.md)) |
| `third_party/linux-reference/` | Upstream source that code is copied from, not compiled |
| `third_party/MacKernelSDK/` | From AirPort_RTW88 |
| `src/compat/` | Linux-API shims inherited from AirPort_RTW88 ([changes](docs/COMPAT-CHANGES.md)) |
| `src/compat_rtw89/` | rtw89 additions: kernel API the upstream headers and driver need (`rtw89_compat.h`), the code behind cfg80211/mac80211 (`rtw89_cfg80211.c`, `rtw89_mac80211.c`), debug log |
| `reference/rtw88_kext/` | AirPort_RTW88's standalone PCI kext glue — starting point for M1 |
| `tools/api_gap.py` | Static Linux-API gap report → [docs/api-gap.md](docs/api-gap.md) |
| `tools/errsum.py` | Groups compiler errors → `docs/compile-status.md` |
| `tools/check_imports.py` | `make link`: what is left for the kernel → `docs/kernel-imports.md` |

## Build loop

On the machine itself (macOS with the Command Line Tools), from the repo root:

```bash
make fetch-firmware   # once
make -k compile       # -k: keep going so every file's errors get recorded
make errors           # writes docs/compile-status.md
make link             # joins all objects, checks the leftover kernel imports
make kext             # userspace smoke test, then build/out/AirPort_RTW89.kext
```

Loading is manual and at your own risk (a driver bug panics the machine):
`sudo tools/load.sh`, then `sudo tools/unload.sh`.

Per-file compiler output is kept in `build/log/`.

## Debug logging

Built with `CONFIG_RTW89_DEBUGMSG`. Pick the categories with the boot-arg
`rtw89_debug=0x...` (bits from `enum rtw89_debug_mask` in
`third_party/rtw89/debug.h`), then read them with
`log show --last boot --predicate 'sender == "AirPort_RTW89"'`.

## License

rtw89 is GPL-2.0 OR BSD-3-Clause; the inherited compat layer and glue are GPL-2.0.
The combined work is GPL-2.0.
