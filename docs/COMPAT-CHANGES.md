# Changes to the inherited AirPort_RTW88 compat layer

`src/compat/` started as a copy of Realtek-AirPort-Family @ 1885e37
(`AirPort_RTW88/Feixiao/src/compat`). Deviations, with ones worth sending upstream marked ⬆.

| File | Change | Upstream? |
|---|---|---|
| `linux/average.h` | `DECLARE_EWMA` used `_weight_rcp` directly as a shift and shifted `val` by `precision - weight`; for `DECLARE_EWMA(rssi, 10, 16)` that is a negative shift (UB; x86 shifts by 58). Now matches Linux: shift by `ilog2(weight)`, blend `(old·(w−1)+val)/w`. Affects rtw88's rssi/evm/snr/thermal/tp averages too. | ⬆ |
| `linux/bitfield.h` | `le64_get_bits` called `u64_encode_bits` (shift left) instead of extracting the field. | ⬆ |
| `fw_blobs.c/.h` | Removed; regenerated from `firmware/` for rtw89. | — |
| `net/mac80211.h` | `struct wiphy` gains `struct mutex mtx` for `wiphy_lock`/`wiphy_work` (rtw89). Must be `mutex_init`ed wherever the wiphy is allocated. | — |
| `linux/skbuff.h` | `struct sk_buff` gains `network_header`/`transport_header`/`mac_header` offsets (rtw89 `ip_hdr`/`udp_hdr`); TX glue must set them. | — |
