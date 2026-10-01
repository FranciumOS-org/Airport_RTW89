# Vendored Linux rtw89

- Source: https://github.com/torvalds/linux `drivers/net/wireless/realtek/rtw89`
- Commit: 551c722 (2026-09-29)
- License: GPL-2.0 OR BSD-3-Clause (per-file SPDX headers)

Only the files needed for RTL8852BE PCIe are vendored (+ all headers).
**Do not edit these files.** macOS differences go in `src/compat_rtw89/` or in
`patches/` as numbered patches applied at build time.
