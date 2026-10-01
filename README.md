# AirPort_RTW89

Work-in-progress port of the Linux **rtw89** driver to macOS for the
**Realtek RTL8852BE** (`10EC:B852`, Wi-Fi 6 PCIe), following the approach of
AirPort_RTW88 in [Realtek-AirPort-Family](https://github.com/xnoah222/Realtek-AirPort-Family).

**Status: M0 (getting it to compile). Nothing loads yet. Do not put this in an EFI.**

See [docs/PORTING.md](docs/PORTING.md) for the plan and milestones.

## Layout

| Path | What |
|---|---|
| `third_party/rtw89/` | Vendored Linux rtw89 subset, **unmodified** ([UPSTREAM.md](third_party/rtw89/UPSTREAM.md)) |
| `third_party/MacKernelSDK/` | From AirPort_RTW88 |
| `src/compat/` | Linux-API shims inherited from AirPort_RTW88 ([changes](docs/COMPAT-CHANGES.md)) |
| `src/compat_rtw89/` | rtw89-specific additions: missing headers, helpers, `wiphy_work`, debug log |
| `reference/rtw88_kext/` | AirPort_RTW88's standalone PCI kext glue — starting point for M1 |
| `tools/api_gap.py` | Static Linux-API gap report → [docs/api-gap.md](docs/api-gap.md) |
| `tools/errsum.py` | Groups compiler errors → `docs/compile-status.md` |

## Build loop (macOS VM with Xcode)

Share the Windows folder `E:\mac development` into the VM (VMware → VM Settings →
Options → Shared Folders), then in the VM's Terminal:

```bash
cd "/Volumes/VMware Shared Folders/mac development/AirPort_RTW89"
make fetch-firmware   # once
make -k compile       # -k: keep going so every file's errors get recorded
make errors           # writes docs/compile-status.md
```

Per-file compiler output is kept in `build/log/`. Because the folder is shared,
the logs and `docs/compile-status.md` are readable from Windows right away.

## Debug logging

Built with `CONFIG_RTW89_DEBUGMSG`. Pick the categories with the boot-arg
`rtw89_debug=0x...` (bits from `enum rtw89_debug_mask` in
`third_party/rtw89/debug.h`), then read them with
`log show --last boot --predicate 'sender == "AirPort_RTW89"'`.

## License

rtw89 is GPL-2.0 OR BSD-3-Clause; the inherited compat layer and glue are GPL-2.0.
The combined work is GPL-2.0.
