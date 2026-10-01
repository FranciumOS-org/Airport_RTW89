# Vendored Linux headers

- Source: https://github.com/torvalds/linux
- Commit: 551c722f40809618230001baccf219193e22fc5a (same pin as `third_party/rtw89`)
- License: per-file SPDX headers (GPL-2.0, some uapi files with the syscall note)

| Here | Upstream path |
|---|---|
| `linux/ieee80211*.h`, `linux/pci_ids.h` | `include/linux/` |
| `uapi/linux/nl80211.h`, `uapi/linux/pci_regs.h` | `include/uapi/linux/` |
| `net/cfg80211.h`, `net/mac80211.h`, `net/regulatory.h`, `net/ieee80211_radiotap.h` | `include/net/` |

**Do not edit these files.** They are on the include path ahead of the inherited
rtw88 shims (see `COMPAT_FLAGS` in the Makefile). Whatever they need from the
rest of the kernel is provided by `src/compat_rtw89/rtw89_compat.h` and the stub
headers next to it. Re-download with `tools/fetch_linux.sh`.

`third_party/linux-reference/` holds upstream source that is not compiled, only
copied from (`net/wireless/util.c` → `src/compat_rtw89/rtw89_cfg80211_bitrate.c`
via `tools/extract_bitrate.sh`).
