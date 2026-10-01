# Changes to the inherited AirPort_RTW88 compat layer

`src/compat/` started as a copy of Realtek-AirPort-Family @ 1885e37
(`AirPort_RTW88/Feixiao/src/compat`). Deviations, with ones worth sending upstream marked ⬆.

| File | Change | Upstream? |
|---|---|---|
| `linux/average.h` | `DECLARE_EWMA` used `_weight_rcp` directly as a shift and shifted `val` by `precision - weight`; for `DECLARE_EWMA(rssi, 10, 16)` that is a negative shift (UB; x86 shifts by 58). Now matches Linux: shift by `ilog2(weight)`, blend `(old·(w−1)+val)/w`. Affects rtw88's rssi/evm/snr/thermal/tp averages too. | ⬆ |
| `linux/bitfield.h` | `le64_get_bits` called `u64_encode_bits` (shift left) instead of extracting the field. | ⬆ |
| `fw_blobs.c/.h` | Removed; regenerated from `firmware/` for rtw89. | — |
| `net/mac80211.h` | **No longer used by rtw89** (it builds against the upstream header, see PORTING.md → Status); kept for reference because `reference/rtw88_kext` was written against it. Earlier change, now moot: `struct wiphy` gained `struct mutex mtx` and the wiphy_work runner fields. | — |
| `linux/skbuff.h` | `struct sk_buff` gains `network_header`/`transport_header`/`mac_header` offsets (rtw89 `ip_hdr`/`udp_hdr`); TX glue must set them. | — |
| `rtw88_compat.h` | Under `RTW89_MACOS`: does not include `net/mac80211.h` (rtw89_compat.h includes the upstream one instead) and skips its own `refcount_t`, `WLAN_*` and `IEEE80211_MAX_QUEUES` definitions, which the upstream `linux/ieee80211.h` and rtw89_compat.h provide. | — |
| `rtw88_compat.c` | Under `RTW89_MACOS`: the rtw88 mini-mac80211 (`ieee80211_*`, `cfg80211_*`, `regulatory_hint`, kext callback table), `get_random_mask_addr` and everything that reaches into `struct rtw_dev` are compiled out. Logging, workqueues, timers, IRQ/NAPI emulation and init/exit stay. | — |
| `linux/etherdevice.h` | `get_random_mask_addr` prototype hidden under `RTW89_MACOS` (static inline in upstream `net/cfg80211.h`). | — |
| `linux/leds.h` | `ieee80211_tpt_blink` / `ieee80211_create_tpt_led_trigger` hidden under `RTW89_MACOS` (upstream `net/mac80211.h` has them). | — |
| `linux/netdevice.h` | `struct net_device` gains `ieee80211_ptr` (read by an inline in upstream `net/cfg80211.h`). | — |
| `linux/skbuff.h` | `skb_put` / `skb_push` / `skb_pull` return `void *` like Linux, not `u8 *`; rtw89 assigns the result to struct pointers without a cast. C callers are unaffected, C++ callers need a cast. | ⬆ |
| `rtw88_firmware.c` | Decompression buffer sized from the image size recorded in the blob table instead of 4× the compressed size, which fails for images that compress better than 4:1. | ⬆ |
| `fw_blobs.h` | Re-added; `struct rtw88_fw_blob` has `uncompressed_size`. `fw_blobs.c` is generated into `build/gen/` by `tools/gen_fw_blobs.py`. | — |
