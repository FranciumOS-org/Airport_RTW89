# AirPort_RTW89 — notes for Claude

Porting Linux rtw89 to a macOS AirPort kext for the RTL8852BE (`10EC:B852`).
Plan, milestones and rationale: docs/PORTING.md. Read it first.

## Machine

Development and hardware testing both happen on the same machine: an ASUS TUF A15
FA507NU (Ryzen 7 7735HS, ~28 GB usable RAM, RTL8852BE, RTL8168 Ethernet) booted
into macOS as a hackintosh. It has no GPU acceleration (no driver for the
Radeon 680M) but boots in under 20 s and is responsive. Internet comes from iPhone USB tethering
(Personal Hotspot over the cable); the RTL8168 Ethernet port (RealtekRTL8111.kext)
is the fallback. The Wi-Fi is what we're building. Bluetooth on the combo card
(USB `13D3:3571`) is handled by RealtekBluetoothFirmware.kext in the EFI, but a
Bluetooth hotspot is slow — use the cable.

## EFI (OpenCore 1.0.8)

Working copy backed up at `setup/EFI-TUF-A15-working` on the Windows E: drive.
This firmware only boots with: DevirtualiseMmio ON + its MmioWhitelist,
SetupVirtualMap OFF (don't change the Booter section). Boot-args:
`debug=0x100 keepsyms=1 -vi2c-force-polling -lilubetaall -wegnoegpu`; add `-v`
only when diagnosing (verbose is very slow without GPU acceleration).
csr-active-config `0xA03` (unsigned kexts allowed). **`ACPI/SSDT-Disable_Network_GPP6.aml`
must stay disabled**: it makes `\_SB.PCI0.GPP6.WLAN` (the RTL8852BE) report
vendor/device 0xFFFF, so our kext could never match. Check with
`ioreg -l | grep -i 'b852'`. Sonoma+ legacy Wi-Fi stack (IO80211FamilyLegacy,
IOSkywalkFamily block, AMFIPass) is only needed from M2.

## Workflow

- Build: `make -k compile && make errors` → read docs/compile-status.md, fix shims in
  batches grouped by category, repeat. Then `make link`: it must report 0 unresolved.
- Never edit `third_party/rtw89/` or `third_party/linux-include/` (vendored upstream,
  see their UPSTREAM.md). Fix things in `src/compat_rtw89/` (new) or `src/compat/`
  (inherited; log every change in docs/COMPAT-CHANGES.md).
- rtw89 builds against the real `net/cfg80211.h` / `net/mac80211.h`. When the driver
  needs a mac80211/cfg80211 function, implement it in `src/compat_rtw89/rtw89_mac80211.c`
  or `rtw89_cfg80211.c` following the Linux behaviour; don't add struct shims.
  `src/compat/net/mac80211.h` (the rtw88 shim) is not used.
- Upstream files: the user has approved downloading further files from the pinned
  Linux commit when needed, as long as each fetched file is named to them. Use the
  GitHub contents API (`tools/fetch_linux.sh`); raw.githubusercontent.com is blocked
  on this connection. Add new files to that script.
- Some files have CRLF line endings (`git ls-files --eol`). Keep each file's endings
  when editing.
- Refresh the static gap report with `python3 tools/api_gap.py` after large shim changes.

## Loading kexts — rules

- **Always ask the user before loading or unloading a kext.** A bad load panics the
  machine and ends this session.
- The user types the sudo password; never ask for it or try to store it.
- Load manually from a running system (`sudo kmutil load -p build/out/...kext`) and
  never install into the EFI or /Library/Extensions until M2 is stable.
- Before any load, commit or stash the work so a panic loses nothing.
- After a reboot: check `/Library/Logs/DiagnosticReports/*.panic` and
  `log show --last boot --predicate 'sender == "AirPort_RTW89"'`.
- Debug output: boot-arg `rtw89_debug=0x...` (mask bits in third_party/rtw89/debug.h).

## Known upstream bugs found (AirPort_RTW88 compat layer)

`DECLARE_EWMA` negative shift (rssi averages garbage) and `le64_get_bits` encoding
instead of extracting — fixed here, see docs/COMPAT-CHANGES.md; not yet reported
upstream.
