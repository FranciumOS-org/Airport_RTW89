# Changes to the inherited AirPort_RTW88 compat layer

`src/compat/` started as a copy of Realtek-AirPort-Family @ 1885e37
(`AirPort_RTW88/Feixiao/src/compat`). Deviations, with ones worth sending upstream marked ⬆.

| File | Change | Upstream? |
|---|---|---|
| `linux/average.h` | `DECLARE_EWMA` used `_weight_rcp` directly as a shift and shifted `val` by `precision - weight`; for `DECLARE_EWMA(rssi, 10, 16)` that is a negative shift (UB; x86 shifts by 58). Now matches Linux: shift by `ilog2(weight)`, blend `(old·(w−1)+val)/w`. Affects rtw88's rssi/evm/snr/thermal/tp averages too. | ⬆ |
| `linux/bitfield.h` | `le64_get_bits` called `u64_encode_bits` (shift left) instead of extracting the field. | ⬆ |
| `fw_blobs.c/.h` | Removed; regenerated from `firmware/` for rtw89. | — |
| `linux/skbuff.h` | `struct sk_buff` gains `network_header`/`transport_header`/`mac_header` offsets (rtw89 `ip_hdr`/`udp_hdr`); TX glue must set them. | — |
| `rtw88_compat.h` | Under `RTW89_MACOS`: does not include `net/mac80211.h` (rtw89_compat.h includes the upstream one instead) and skips its own `refcount_t`, `WLAN_*` and `IEEE80211_MAX_QUEUES` definitions, which the upstream `linux/ieee80211.h` and rtw89_compat.h provide. | — |
| `rtw88_compat.c` | Under `RTW89_MACOS`: the rtw88 mini-mac80211 (`ieee80211_*`, `cfg80211_*`, `regulatory_hint`, kext callback table), `get_random_mask_addr` and everything that reaches into `struct rtw_dev` are compiled out. Logging, workqueues, timers, IRQ/NAPI emulation and init/exit stay. | — |
| `linux/etherdevice.h` | `get_random_mask_addr` prototype hidden under `RTW89_MACOS` (static inline in upstream `net/cfg80211.h`). | — |
| `linux/leds.h` | `ieee80211_tpt_blink` / `ieee80211_create_tpt_led_trigger` hidden under `RTW89_MACOS` (upstream `net/mac80211.h` has them). | — |
| `linux/netdevice.h` | `struct net_device` gains `ieee80211_ptr` (read by an inline in upstream `net/cfg80211.h`). | — |
| `linux/skbuff.h` | `skb_put` / `skb_push` / `skb_pull` return `void *` like Linux, not `u8 *`; rtw89 assigns the result to struct pointers without a cast. C callers are unaffected, C++ callers need a cast. | ⬆ |
| `rtw88_firmware.c` | Decompression buffer sized from the image size recorded in the blob table instead of 4× the compressed size, which fails for images that compress better than 4:1. | ⬆ |
| `fw_blobs.h` | Re-added; `struct rtw88_fw_blob` has `uncompressed_size`. `fw_blobs.c` is generated into `build/gen/` by `tools/gen_fw_blobs.py`. | — |
| `linux/pci.h` | `pcie_capability_set_word` / `clear_word` wrote to config offset `0x100 + pos` (the first *extended* capability) and `pcie_capability_read_word` read raw offset `pos`. All three now locate the PCI Express capability (ID 0x10) and add `pos` to it, as Linux does. | ⬆ |
| `rtw88_firmware.c` | A decompress that did not reach `Z_STREAM_END` returned zlib's code, which can be `Z_OK` (0), so the caller took a half-filled buffer for a loaded image. Now always an error. | ⬆ |
| `rtw88_compat.c` | Timers: every `thread_call` a timer allocates is tracked, and `rtw88_compat_exit()` cancels and frees the ones never deleted with `del_timer_sync()`, so none can fire into an unloaded kext. | ⬆ |
| `rtw88_compat.c` | Log prefix is `rtw89` under `RTW89_MACOS`. | — |
| `linux/slab.h`, `rtw88_compat.c` | `kfree()`'s "not a kernel pointer" threshold is the variable `rtw88_kfree_min_addr` (same default) so the userspace smoke test can lower it. | — |
| `linux/skbuff.h` | `skb_copy` allocated only `len + headroom` (+64 bytes of slack), so the copy lost the original's tailroom; rtw89 appends probe-request IEs to such a copy (`rtw89_append_probe_req_ie`) and would have written past the buffer. It now keeps head- and tailroom and the metadata fields, like Linux. | ⬆ |
| `linux/skbuff.h`, `rtw88_compat.c` | `skb_put` / `skb_push` check the room and stop (`rtw88_skb_panic`) instead of silently corrupting the heap, as Linux's `skb_over_panic` does. | ⬆ |
| `linux/skbuff.h` | `dev_alloc_skb` / `netdev_alloc_skb` reserve `NET_SKB_PAD` (64) bytes of headroom like Linux. | — |
| `rtw88_compat.c` | New hook `rtw88_napi_post_poll` (called on the datapath thread after every NAPI poll) and `rtw88_queue_datapath_delayed_work()` (queue work on that thread). rtw89 uses them to pass on received frames the driver is holding for a PPDU status report that does not come; see `src/compat_rtw89/rtw89_core_wrap.c`. | — |

## Single-target Instant Hotspot scans

Preserve a nonempty SSID from either native scan request format and translate
a single requested 2.4/5/6 GHz channel into the actual firmware scan. Directed
scans bypass the connected background throttle and channel slicing, keeping
regulatory channel checks. While a directed scan runs, report Busy rather than
returning the old scan cache. A different concurrent directed request returns Busy. Identical retries share
the in-flight scan and retain its completed results until the list is consumed.
This handles ioctl clients that resubmit SCAN_REQ after a Busy SCAN_RESULT,
which otherwise start an endless sequence of new scans.

Ordinary wildcard scans retain their throttle and channel slicing; directed
scans do not advance the background scan cursor. Multi-name background requests
retain their existing wildcard behavior. An unspecified or multi-channel target
request scans all eligible channels. SAE/H2E/PMF and association policy are unchanged.

Regression coverage checks native request decoding (including embedded NUL SSID
bytes and overlapping 2.4/6 GHz channel numbers) and reads back the actual
cfg80211 scan request in the connected fake-chip harness, checking the target,
frequency, one-channel scan, invalid inputs, and preserved connection.

## Sequoia scan ioctl wire ABI

Actual IO80211.framework requests on macOS 15.8.1 use 0x802869c8/0xc02869c9
(40-byte envelopes), while the imported header constants encode 48 bytes.
Recognize the 40-byte commands only for SCAN_REQ, SCAN_REQ_MULTIPLE and
SCAN_RESULT so these operations reach the BSD bridge. Association and other
requests keep their existing family path.

The observed 5528-byte single scan has its channel count/list at 68/72. The
5352-byte multiple scan has ten SSID slots and channel count/list at 528/532,
not the SDK's sixteen slots and 768/772. Normalize these explicitly recognized
wire layouts to the legacy single-target representation before decoding.
Validate buffer size, version, SSID lengths and channel bounds. Multi-name
and multi-channel requests retain the previously documented wildcard/full
scan behavior. Tests cover raw wire fixtures independently of SDK layout,
plus optional replay of a locally captured hotspot payload (not committed).
