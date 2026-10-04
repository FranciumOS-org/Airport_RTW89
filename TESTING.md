# Testing AirPort_RTW89

Thank you for trying it. Only the RTL8852BE has run this driver on real
hardware so far; your report decides whether your card works. Expect the
first boot on a new card to go wrong in some way: that is useful too.

## Before you start

1. **Make sure you can boot without it.** Copy your working OpenCore EFI to a
   USB stick and boot from it once. If macOS does not start after installing,
   boot from the stick: it does not have this driver.
2. Download both zips of the release, `AirPort_RTW89-<version>.zip` (the
   kext) and `AirPort_RTW89-<version>-tools.zip`; unpack the tools and put
   `AirPort_RTW89.kext` in that folder. From it, run
   ```sh
   sudo tools/collect_logs.sh
   ```
   `logs/system.txt` now shows your card's PCI ID (`device-id=...` on the
   Wi-Fi line) and your macOS version. Check the card is in the table in the
   [README](README.md#cards).
3. Install as the [README](README.md#installing) says, preferably with
   setup (`setup.command` on macOS, `setup.cmd` on Windows), and restart.

## What to try

Note what happens at each step; stop at the first one that fails and report.

1. **Boot.** Does macOS start? If it panics or hangs, boot from the USB stick
   and run `sudo tools/collect_logs.sh` there: it picks up the panic report.
2. **Wi-Fi appears.** Within about 30 seconds of the desktop, is there a Wi-Fi
   icon in the menu bar, and does it list networks?
3. **Join** your WPA2 (or WPA2/WPA3) network. Does it connect, and do web pages
   load?
4. **Speed**, with any speed test site: once next to the router, once a room
   or two away. Note the band (2.4 or 5 GHz) and, if you can, what the same
   spot gets in Windows or Linux.
5. **Restart** and **shut down** once each: does Wi-Fi come back by itself?
6. **Sleep** (Apple menu -> Sleep), wait a minute, wake it: does Wi-Fi come
   back? (Untested so far: no machine here really sleeps.)
7. If you have them: a phone's Personal Hotspot, an 802.1X network (work,
   school).

Then run `sudo tools/collect_logs.sh` once more, while Wi-Fi is in the state
you are reporting.

## What to send

Open an issue at <https://github.com/FranciumOS-org/Airport_RTW89/issues> titled with your card and the
result (e.g. "RTL8852CE: no networks listed"), and attach the zip that
`collect_logs.sh` names (`logs/AirPort_RTW89-report-....zip`) with this,
filled in:

```
Card:            (e.g. RTL8852CE, 10ec:c852)
Machine:         (laptop model or motherboard, CPU)
macOS:           (e.g. 15.8.1)
OpenCore:        (e.g. 1.0.8)
1 Boot:          ok / panic / hang
2 Wi-Fi appears: yes / no
3 Join:          yes / no (network security: WPA2, WPA2/WPA3, ...)
4 Speed:         near: __ Mb/s on __ GHz; far: __ Mb/s on __ GHz (other OS: __)
5 Restart/off:   ok / ...
6 Sleep/wake:    ok / ...
7 Other:         ...
```

The zip has no passwords, and MAC addresses in it are cut to their first
half. Network names can appear in it: open the files and look before posting
it anywhere public, or send it privately.

## Going back

`sudo efi/uninstall.sh` switches the driver and the old Wi-Fi stack off in
your config (the files stay; `sudo efi/install.sh` switches them on again).
Your config as it was before the first install is kept as
`EFI/OC/config.plist.pre-airport-rtw89`.
