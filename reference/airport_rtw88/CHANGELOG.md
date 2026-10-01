# 2.0.0

- Reworked AWDL around an AirportItlwm-style IO80211 control plane plus an OpenAWDL-derived over-air scheduler.
- IO80211 sync templates, channel hints, sync parameters, presence/election state are now treated as authoritative policy; the Realtek backend owns only safe PHY/channel execution.
- Added a 1.5 s IO80211 grace period before native/OpenAWDL bootstrap, with immediate takeover when an Apple sync template appears.
- AWDL BPF management/action frames are now queued and transmitted only inside an owned AWDL availability window instead of being injected immediately on the infrastructure channel.
- A valid Apple AWDL BPF action can seed the runtime sync template when Ventura supplies it through BPF before the dedicated template selector.
- AWDL channel/master/sequence IOCTLs no longer retune the single RTL88 PHY directly; the STA/AWDL timeslicer performs all physical channel changes.
- Added IORegistry telemetry for control-plane ownership and Apple action-frame queue/TX state.
- Added `tests/test_v200_awdl_airportitlwm_bridge.py`.
- Added explicit IONetworkController/IOKit power-management registration.
- Added hibernation-safe cold reinitialization of the RTL88 PCI function.
- Quiesce order: AWDL -> scan/auth -> rtw88 core -> IRQ/DMA -> PCI memory.
- Resume order: PCI memory/bus master -> rtw88 core/firmware -> IRQ -> AWDL discovery.
- Never resurrect stale pre-hibernate association/key state.
- Added PM telemetry and regression test `tests/test_v200_hibernation.py`.
- Preserves v53-v57 datapath/AWDL work.

# Changelog

## 1.0.1

- Expanded PCIe device-ID coverage across the supported RTL88xx family mappings.
- Added/updated embedded firmware coverage for the enabled PCIe chipsets.
- Fixed overlapping CoreWiFi scans by coalescing requests instead of returning busy/error 16.
- Corrected the bundled Ventura `apple80211_scan_result` ABI to match the layout expected by airportd.
- Retains the stable native AirPort/IO80211 STA path from 1.0.0.
- Retains CURRENT_NETWORK / Location Services compatibility work from the late 1.0.0 development branch.
- AWDL/P2P code remains experimental and is not a supported 1.0.1 feature.

### Hardware validation

RTL8822BE is the physically validated adapter. Other enabled PCIe IDs remain experimental until confirmed on real hardware.
