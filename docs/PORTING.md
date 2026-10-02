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

**Towards M3: association and the WPA2 key handshake work on hardware.**
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
On the test machine the handshake with a WPA2 access point completed about 5 ms
after the AP's first message: both keys accepted by the chip, link "connected",
clean unload afterwards.
Not checked yet: that the RSN element in message 3 equals the one in the beacon
(a downgrade check that matters once more than one cipher/AKM is supported).

Networks that are WPA3-only, enterprise, WEP/WPA1, or require management frame
protection are refused with a message.

**M3 reached: data path and network interface work on hardware.** On the test
machine the kext loaded as a network controller, joined a WPA2 network on
5220 MHz, macOS got a DHCP lease on the new `en4` and pinged the router (5/5,
1-4 ms), and the kext unloaded cleanly. macOS does not create a network service
for the new port by itself: `sudo ifconfig en4 up; sudo ipconfig set en4 DHCP`
(or adding the port in System Settings once) starts DHCP.

`src/compat_rtw89/rtw89_data.c` converts between Ethernet and 802.11 data
frames, following mac80211's tx.c and rx.c. Transmit: QoS header with the TID
taken from the DSCP field, per-TID sequence numbers, SNAP encapsulation, the
CCMP header with the next packet number (the chip encrypts), then onto the
station's TXQ, which the driver's own TX work drains. Receive: address and
duplicate checks, the CCMP packet number must advance per key and TID (group
keys start from the RSC the AP gave in the handshake), A-MSDU subframes,
conversion back to Ethernet; EAPOL goes to the MLME, everything else to the
network stack, and only once the handshake is done. Unencrypted data on an
encrypted network is dropped in both directions.

The kext is now an `IOEthernetController`: macOS sees an Ethernet port (`enX`)
whose link comes up when a network is joined and its keys are installed, and
configures it with DHCP. This needs no EFI change and no IO80211; the price is
that there is no Wi-Fi menu and networks are joined with `rtw89ctl`.

Objects the driver's threads may still be using when they are removed
(stations, their TXQs, keys) are unlinked at once and freed two seconds later
or when the driver stops: a stand-in for the RCU grace period Linux relies on.

**Aggregation works on hardware.** On the test machine (802.11n, 20 MHz, two
streams, signal around -23 dBm) downloads through the card ran at about
92 Mb/s, measured with three 50 MB transfers and confirmed by the driver's own
packet counters; the AP opened receive sessions on TIDs 0-7 and accepted ours.
An upload reached about 33 Mb/s, with 343 frames dropped by the transmit queue
(tail drop at its limit) over two 20 MB uploads, which TCP answers by slowing
down. Since then the kext uses the pull model for output
(`configureOutputPullModel`, `outputStart`): the network stack keeps the queue,
is told to stop when 256 frames are waiting on the driver's TXQs and is woken
when they have drained to 64. That part has not run on hardware yet.

With that, uploads ran at 85-96 Mb/s with no drops and downloads at about
80 Mb/s (three runs each); on the next build 94-96 Mb/s down and 56-88 up.

**Latency of larger received frames (fixed, confirmed on hardware).**
Small pings took 2 ms, but replies of 150 bytes and more took anything from 1
to 98 ms, evenly spread: they were handed to the stack when the *next* frame
was received, usually the AP's beacon (102 ms interval). Transmit was fine at
every size (measured one way, between the card and the wired port of the same
machine). `rtw89_core_rx()` parks each data frame until the chip's PPDU status
report for the same transmission arrives and otherwise flushes on the next
reception. On this card the report does not come for most data frames: with
the fix in, 49,520 of 127,000 frames of a download were released by it rather
than by a report, and round trips with 1000-byte replies went from a 15-22 ms
median (up to 98 ms) to 3.3 ms (worst 9 ms). Why the reports are missing is
not known; beacons and other low-rate frames get theirs.
`src/compat_rtw89/rtw89_core_wrap.c` compiles the driver's
core.c unchanged and adds a flush the glue runs 1 ms (at first 2 ms) after a
poll that left frames parked (`rtw89ctl flush off` disables it, for comparison). Timing
counters in `rtw89ctl status`, based on the chip's receive timestamps, show
how many frames reached the driver late and whether the chip had interrupted
when they arrived. Also fixed on the way: the stand-in left the RTS threshold
at 0 instead of "off".

The aggregation sessions are BlockAck agreements in both
directions, following agg-rx.c, agg-tx.c and the reorder code in rx.c. The AP's
ADDBA requests are accepted (up to 64 frames, A-MSDU allowed) and its frames go
through a per-TID reorder buffer: in order they pass straight through, behind a
gap they wait up to 100 ms, a BlockAck request or a frame beyond the window
moves it on. In the other direction the driver asks for a session once it has
traffic on a TID; the MLME holds that TID's TXQ, sends the ADDBA request with
the sequence number of the first frame held back, and lets the frames go when
the AP has answered (or after a second without an answer). DELBA from either
side ends a session, and all of them are torn down before the station goes.

**Wide channels and 802.11ac (confirmed on hardware).** The channel
is taken from the AP's HT and VHT operation elements as
`ieee80211_determine_ap_chan()` does: 40 MHz from the secondary channel offset,
80 MHz from the VHT centre frequency, checked against each other and against
the channels the regulatory domain allows, and narrowed step by step if not
usable (an AP on 160 MHz is joined on the 80 MHz half with the control
channel). The association request then carries the 40 MHz bits and a VHT
capabilities element, and the station entry gets the AP's VHT capabilities,
bandwidth and stream count for the firmware's rate control. `rtw89ctl scan`
shows each network's mode, width and security; `status` the channel in use.

On the test machine the join came up as 802.11ac, 80 MHz (centre 5210 MHz), two
streams, at a signal of about -18 dBm. Between the card and the wired port of
the same machine (`tools/tput_probe.py`, TCP, 5 s runs, so the internet is not
in the way) it carried 474-509 Mb/s down and 507-527 Mb/s up, with no frames
dropped on transmit and no reorder timeouts; 0.5-1% of received frames were
retransmissions the driver had already seen. Internet transfers through the
card ran at 258-360 Mb/s down and about 200 Mb/s up, which is the connection's
limit rather than the card's (the wired port did 100 Mb/s up at that moment).
Round trips took a median of 1.3-2.4 ms at every size (worst 10 ms).

**802.11ax (confirmed on hardware).** A network that has the HE
capabilities and HE operation elements is joined as 802.11ax, on 5 GHz on top
of 802.11ac and on 2.4 GHz on top of 802.11n, after the checks
`ieee80211_determine_chan_mode()` makes: the AP's HE rates are consistent, the
card can do the rates the AP requires of every station, the channel is not
flagged NO_HE. The association request carries the card's HE capabilities
(`ieee80211_put_he_cap()`, cut to the width in use). From the response the
station entry gets the AP's HE capabilities (`ieee80211_he_cap_ie_to_sta_he_cap()`)
and the link its BSS colour, default packet extension, RTS duration threshold,
UORA and spatial reuse parameters, and the MU EDCA parameters go to the driver
with the WMM ones. Both HE elements must be in the association response, as in
Linux; otherwise the join stays 802.11ac. BlockAck: requests to an 802.11ax AP
ask for 128 frames (the chip's limit) instead of 64 and carry the ADDBA
extension element; the receive window stays at 64 frames, the chip's limit.
`rtw89ctl ax off` makes the next join stop at 802.11ac, for comparing.

On the test machine the same network came up as 802.11ax, 80 MHz, two streams.
Against the wired port (`tools/tput_probe.py`) uploads rose to 504-670 Mb/s;
downloads stayed where 802.11ac had them, at 477-518 Mb/s, so something other
than the radio rate limits that direction (the 64-frame receive window is one
candidate; the machine's processors are not busy). Round trips: median
1.2-1.7 ms. Internet downloads 355-388 Mb/s against 633-675 Mb/s on the wired
port. During the downloads about 0.3% of frames reached the driver more than
5 ms after the chip's receive timestamp (worst 19 ms), which did not happen
with 802.11ac; whether that is real delay or the timestamp of a long 802.11ax
transmission being that of its start is not known yet.

**Keeping the connection (confirmed on hardware, as far as it can be provoked).**
`rtw89ctl probe`: the AP acknowledged the null frame and the link stayed up, so
the driver's transmit status works here as it does under Linux. `rtw89ctl
drop`: joined again 4.5 s later (0.5 s pause, 3.7 s scan, 0.2 s join and
handshake), and macOS renewed the address on the interface by itself. A real
loss of the AP, a channel switch and a parameter change in a beacon have only
been seen in the smoke test. The rates now shown explain nothing about the
download ceiling: both directions run at 802.11ax MCS 10-11 on two streams
(1081-1201 Mb/s) while downloads stay at 460-500 Mb/s and uploads reach
635-650 Mb/s.

- *Is the AP still there?* The firmware watches the beacons (the driver sets
  CONNECTION_MONITOR) and reports when they stop. As `ieee80211_mgd_probe_ap()`
  does, the MLME then sends a null data frame, which the AP must acknowledge;
  the driver's transmit status says whether it did. Acknowledged, or a beacon
  arrives: nothing happened. Not acknowledged twice, or no status within half
  a second: the connection is given up.
- *Beacons.* The AP's beacons heard on its own channel go to the MLME as well
  as to the scan list. Of `ieee80211_rx_mgmt_beacon()` this does: protection,
  preamble and slot time (`ieee80211_handle_bss_capability()`), new versions of
  the WMM and MU EDCA parameters, and the HT operation mode. A hash of the
  elements that matter skips beacons that say nothing new.
- *A changed channel.* Linux follows a channel switch announcement and changes
  width with the AP. Here the connection is dropped instead, when the announced
  switch is due or when the beacons describe another channel than the first
  one after the join did, and joined again.
- *Coming back.* The glue remembers the network once a join has worked (name
  and key). When the connection is lost for any reason other than `leave` or
  `down`, it scans and joins again, with pauses growing from half a second to
  a minute between attempts. A join that never worked (wrong password) is not
  repeated. `rtw89ctl rejoin off` disables it.
- `rtw89ctl status` shows the rate the firmware sends at, the rate of the last
  frame received from the AP, beacon counters and how often the connection has
  come back. `rtw89ctl probe` and `rtw89ctl drop` trigger the probe and a
  reconnection by hand, to test them on the card.

Not there yet:
- Of the beacon: BSS colour changes, channel switches followed rather than
  rejoined, the DTIM period, power constraints, and channel switch
  announcements in action frames.
- Target wake time, and on 5 GHz an 802.11ax AP without 802.11ac elements
  (joined as 802.11n).
- Fragmented frames (dropped), software decryption of frames the chip did not
  decrypt (dropped and counted), power save, and roaming between access points
  of one network while connected (after a loss the strongest one is joined).

**Native Wi-Fi: the route.** The Ethernet-style interface is not Wi-Fi to
macOS. For the menu, System Settings and location services the kext has to
drive Apple's IO80211 family, and the choice (2026-10-01) is the one that
exists up to Ventura and that OpenCore can put back on Sonoma to Tahoe:
`IO80211FamilyLegacy` on Ventura's `IOSkywalkFamily`, see `efi/README.md`. The
reference driver shows that Sequoia's and Tahoe's own Wi-Fi software can talk
to it unpatched (it translates their 900- and 908-byte association requests),
so no OCLP root patch is planned; to be confirmed at the first scan here.
Writing against Sequoia's own IO80211Family instead was looked at and dropped:
no EFI change and no reboot per test, but tied to one macOS version, and
nobody has done it (`tools/kcvtab.py` reads the class layouts it would need
from the kernel collections).

**Native Wi-Fi works on hardware (2026-10-01).** macOS lists the card as
Wi-Fi (`en4`), its own software scans, and joining Golestan-5G from the Wi-Fi
menu gave an 802.11ax link with an address and pings at 2-3 ms. No OCLP root
patch: three kexts in the EFI and the block of Apple's IOSkywalkFamily are
enough on Sequoia 15.8.1.

How it is put together, because of two things found on the way:

- `kmutil` will not load a kext that depends on a family OpenCore injected
  (`tools/deptest.sh`). So the IO80211Controller is a small kext of its own,
  `AirPortRTW89Front` (`src/front/`), injected by OpenCore next to the family.
  It attaches to the card, creates the Wi-Fi interface and passes every request
  on through a table of C functions (`src/front/rtw89_front_api.h`).
- The driver proper stays what it was, loaded with `tools/load.sh`. If it finds
  the front in the registry it connects to it instead of publishing its own
  Ethernet interface (`src/kext/AirPortRTW89Native.cpp`: the requests of
  AirPort_RTW88's controller, answered from the glue). Without the front it
  behaves as before. Only a change to the front needs a restart.
- OpenCore itself boots from a USB stick macOS cannot read; a second copy on
  the internal EFI partition is the one that carries the front
  (`efi/install_internal.sh`, `efi/install_front.sh`). The stick is the way
  back.

The first run double-faulted: IO80211 re-enters `apple80211Request()` from
inside a power change, and the handler had 5.8 KB of locals. Its buffers are
on the heap now.

Not there yet in native mode: sleep and wake, AWDL (AirDrop), scans while
connected (answered from what has been heard), WPA3, and loading the driver
at boot (it is still loaded by hand).

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
  connected. Once connected it sends and receives data frames and checks the
  conversion byte for byte, along with replayed, duplicated, unencrypted,
  undecrypted, misaddressed and A-MSDU frames, group keys by id, and that
  nothing passes before the handshake or after leaving. Then BlockAck
  sessions: frames arriving out of order, twice, never, or far ahead; a
  BlockAck request; the driver's own ADDBA request answered with yes, no and
  silence; and that a TID's frames wait while its session is negotiated. The
  pretend device consumes firmware commands so that the command ring does not
  fill up over the many joins.

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
- `tools/rtt_probe.py`, `tools/tput_probe.py` — round-trip times and TCP throughput through the card, measured against the wired port of the same machine.
- `tools/rtw89ctl.c` — built by `make kext` as `build/out/rtw89ctl`: `up`, `scan`, `join`, `leave`, `down`, `status`, `flush on|off`, `ax on|off` for a loaded kext.
