# Credits

AirPort_RTW89 stands on the work of these projects and their contributors.

- **Linux rtw89** (Realtek and the Linux wireless developers): the driver for
  the chip itself, vendored unmodified in `third_party/rtw89/`, and the 802.11
  headers in `third_party/linux-include/`. Parts of Linux's mac80211 and
  cfg80211 were followed closely in `src/compat_rtw89/`.
- **AirPort_RTW88** and **Realtek-AirPort-Family**: the Linux-API shims in
  `src/compat/`, the kext glue this port started from, MacKernelSDK, and the
  reference for macOS's newer Wi-Fi request formats.
- **OpenIntelWireless** (itlwm, AirportItlwm): how a third-party driver works
  with Apple's IO80211 family.
- **OpenCore Legacy Patcher** and **Acidanthera**: the old Wi-Fi stack
  (`IOSkywalkFamily`, `IO80211FamilyLegacy`) as a loadable set, and AMFIPass;
  OpenCore and Lilu.
- **hostap** (wpa_supplicant/hostapd) and **Mbed TLS**: vendored for WPA3.
- **Realtek**: the chip's firmware, from linux-firmware.
