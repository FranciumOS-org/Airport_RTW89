# Vendored Linux rtw89

- Source: https://github.com/torvalds/linux `drivers/net/wireless/realtek/rtw89`
- Commit: 551c722 (2026-09-29)
- License: GPL-2.0 OR BSD-3-Clause (per-file SPDX headers)

The core, all headers, and the chip and PCIe front-end files of every rtw89
PCIe card: RTL8851BE, RTL8852AE, RTL8852BE, RTL8852BTE, RTL8852CE, RTL8922AE,
RTL8922DE (the chip files added 2026-10-03 from the same commit by
tools/fetch_linux.sh). The USB front ends and debug.c/wow.c are not vendored.
**Do not edit these files.** macOS differences go in `src/compat_rtw89/` or in
`patches/` as numbered patches applied at build time.
