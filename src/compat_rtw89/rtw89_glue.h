/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Boundary between the platform (the IOKit kext, or the host smoke test) and
 * everything Linux-shaped. This header is plain C with no Linux types, so the
 * C++ side never sees the compat headers; rtw89_glue.c translates.
 *
 * The platform supplies config-space access, the mapped register BAR and DMA
 * memory. The glue builds the struct pci_dev, installs the compat PCI/DMA ops
 * and runs the driver's own probe()/remove().
 */
#ifndef _RTW89_GLUE_H
#define _RTW89_GLUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rtw89_glue_platform {
    void *ctx;

    /* PCI configuration space; @offset up to 4095, @width 1, 2 or 4 bytes. */
    uint32_t (*cfg_read)(void *ctx, unsigned int offset, unsigned int width);
    void (*cfg_write)(void *ctx, unsigned int offset, unsigned int width,
                      uint32_t value);

    /*
     * Wired, physically contiguous memory the device can reach (below 4 GB).
     * *@bus_addr is the address to program into the device; *@cookie is handed
     * back to dma_free(). May sleep.
     */
    void *(*dma_alloc)(void *ctx, size_t size, uint64_t *bus_addr, void **cookie);
    void (*dma_free)(void *ctx, void *cookie);

    /* Let the device's interrupt reach rtw89_glue_interrupt(), or stop it. */
    void (*irq_enable)(void *ctx, bool enable);

    /* Optional: the link state changed (see rtw89_glue_link()). Called from a
     * driver thread with driver locks held: do not call back into the glue
     * other than rtw89_glue_link(). */
    void (*link_changed)(void *ctx);

    /* Optional: a received Ethernet frame (destination, source, type,
     * payload) for the network stack. Called from the driver's receive
     * thread, may sleep; @frame is only valid during the call. */
    void (*rx_frame)(void *ctx, const uint8_t *frame, size_t len);

    /* Optional: rtw89_glue_tx_room() returned zero earlier and there is room
     * again. Called from a driver thread; may call rtw89_glue_tx*(). */
    void (*tx_wake)(void *ctx);

    /* Optional: a scan has ended (or was given up). Called from a driver
     * thread with driver locks held: only the scan result calls are safe. */
    void (*scan_done)(void *ctx, bool aborted);
};

struct rtw89_glue_device {
    uint16_t vendor;
    uint16_t device;
    uint16_t subsystem_vendor;
    uint16_t subsystem_device;
    uint8_t  revision;

    /* The memory BAR the driver uses for its registers (BAR 2), mapped. */
    volatile void *mmio_base;
    size_t mmio_len;
};

struct rtw89_glue_info {
    uint8_t  mac[6];
    char     fw_version[32];    /* "0.29.29.18"; empty if not recognised */
    uint32_t fw_commit;
    uint8_t  chip_cut;          /* 0 = A, 1 = B, ... */
    uint8_t  rfe_type;
    uint8_t  tx_streams;
    uint8_t  rx_streams;
};

/* True if the driver's id_table has this vendor/device pair. */
bool rtw89_glue_supports(uint16_t vendor, uint16_t device);

/*
 * Bring the compat runtime up and run the driver's probe(): allocate the hw,
 * power the chip on, download firmware, read the efuse, power it off again and
 * register. Returns 0 or a negative Linux errno. One device at a time.
 */
int rtw89_glue_probe(const struct rtw89_glue_platform *platform,
                     const struct rtw89_glue_device *device);

/* Undo rtw89_glue_probe(). Safe to call if probe failed or never ran. */
void rtw89_glue_remove(void);

/* The device raised its interrupt. Called from a thread, never from the
 * primary interrupt: the handler takes sleeping locks. */
void rtw89_glue_interrupt(void);

/* Valid between a successful probe and remove. */
bool rtw89_glue_get_info(struct rtw89_glue_info *info);

/* Linux's Wi-Fi/Bluetooth coexistence report (debugfs btc_info) into @buf;
 * returns its length, 0 if the driver has not probed. */
size_t rtw89_glue_coex_info(char *buf, size_t max);

/*
 * Radio up/down: what mac80211 does when the first interface is opened.
 * up() starts the hardware (power on, firmware, calibration, interrupts) and
 * adds one station interface with the card's own address; down() reverses it.
 * Both may sleep for a long time. Not reentrant: serialise calls.
 */
int rtw89_glue_up(void);
void rtw89_glue_down(void);
bool rtw89_glue_is_up(void);

/*
 * Start a firmware scan over every enabled channel: active (wildcard probe
 * request) where regulations allow, passive elsewhere. Returns 0 once the scan
 * is running; it finishes on its own after a few seconds. -EBUSY if one is
 * already running, -ENETDOWN if the radio is down.
 */
int rtw89_glue_scan(void);
bool rtw89_glue_scanning(void);

struct rtw89_glue_bss {
    uint8_t  bssid[6];
    uint8_t  ssid_len;
    char     ssid[33];          /* NUL-terminated; not necessarily printable */
    uint16_t freq;              /* MHz */
    uint8_t  channel;
    int8_t   signal;            /* dBm */
    uint16_t capability;
    uint32_t seen;              /* beacons and probe responses heard */
    uint8_t  mode;              /* 0: 802.11a/b/g, 1: n, 2: ac, 3: ax */
    uint8_t  width;             /* MHz the AP operates on */
    uint8_t  security;          /* RTW89_GLUE_SEC_* bits; 0: open */
};

#define RTW89_GLUE_SEC_WEP_WPA1     0x01    /* encrypted, but not RSN: WEP or WPA1 only */
#define RTW89_GLUE_SEC_WPA2_PSK     0x02
#define RTW89_GLUE_SEC_WPA3_SAE     0x04
#define RTW89_GLUE_SEC_ENTERPRISE   0x08    /* 802.1X */
#define RTW89_GLUE_SEC_PMF_REQUIRED 0x10    /* management frame protection required */

/* Networks heard since the last rtw89_glue_scan() (beacons and probe
 * responses). Returns how many were copied to @out. */
unsigned int rtw89_glue_scan_results(struct rtw89_glue_bss *out, unsigned int max);

/*
 * One network of that list, with what a caller needs to describe it fully:
 * the elements of its last beacon or probe response (up to @ies_max bytes,
 * *@ies_len says how many) and its beacon interval. @index counts as in
 * rtw89_glue_scan_results(); returns false past the end. rtw89_glue_find_bss()
 * looks one up by address instead.
 */
bool rtw89_glue_scan_entry(unsigned int index, struct rtw89_glue_bss *out, uint8_t *ies,
                           size_t ies_max, size_t *ies_len, uint16_t *beacon_int);
bool rtw89_glue_find_bss(const uint8_t bssid[6], struct rtw89_glue_bss *out, uint8_t *ies,
                         size_t ies_max, size_t *ies_len, uint16_t *beacon_int);

/* The channels the card and the regulatory domain allow. */
struct rtw89_glue_channel {
    uint16_t freq;              /* MHz */
    uint8_t  channel;
    bool     passive;           /* listen only: no probe requests */
    bool     radar;
};
unsigned int rtw89_glue_channels(struct rtw89_glue_channel *out, unsigned int max);

/*
 * Use @mac as the station's address instead of the card's own (macOS gives
 * each network a private address). Takes the radio down and up again if it is
 * up, which ends any connection. Returns 0 or a negative errno.
 */
int rtw89_glue_set_mac(const uint8_t mac[6]);

/*
 * Join the strongest network heard under @ssid in the last scan. @passphrase
 * is the WPA2 password (8 to 63 characters, or 64 hex digits for a raw key);
 * NULL or empty for an open network. Returns 0 once the attempt has started;
 * it completes in the background (see rtw89_glue_link()). -ENOENT if no such
 * network was heard, -EACCES if it needs a password and none (or an invalid
 * one) was given, -EOPNOTSUPP if its security is not something this driver
 * can do.
 */
int rtw89_glue_join(const uint8_t *ssid, size_t ssid_len,
                    const char *passphrase, size_t passphrase_len);
/*
 * Join a network with 802.1X sign-in (WPA2-Enterprise). The glue only
 * associates: the sign-in is done by whoever sends and receives the EAP
 * frames on the data path. What it produces is handed over either as the
 * pairwise master key with rtw89_glue_set_pmk() (32 bytes), after which the
 * key handshake is done here, or, by a supplicant that has done the handshake
 * itself, as keys with rtw89_glue_set_key() (CCMP, 16 bytes; @rsc: the group
 * key's receive counter). EAPOL-Key frames are not passed up. @rsn_ie: the
 * RSN element to associate with, or NULL. The link counts as connected once
 * associated; data other than EAPOL flows when both keys are in
 * (rtw89_glue_link()'s authorized).
 */
int rtw89_glue_join_ext(const uint8_t *ssid, size_t ssid_len, const uint8_t *bssid,
                        const uint8_t *rsn_ie, size_t rsn_len);
int rtw89_glue_set_key(bool pairwise, int index, const uint8_t *key, size_t len, uint64_t rsc);
int rtw89_glue_set_pmk(const uint8_t *pmk, size_t len);
/* The RSN element this side put in its association request; returns its length. */
size_t rtw89_glue_assoc_rsn_ie(uint8_t *buf, size_t max);

/* The same with the 32-byte pairwise master key already derived (NULL: an
 * open network), and optionally one access point picked by @bssid. */
int rtw89_glue_join_pmk(const uint8_t *ssid, size_t ssid_len, const uint8_t *bssid,
                        const uint8_t *pmk);
/* WPA2-PSK's pairwise master key from the network's name and its passphrase
 * (8-63 characters, or the key itself as 64 hex digits). -EACCES if the
 * passphrase is not one. */
int rtw89_glue_derive_pmk(const uint8_t *ssid, size_t ssid_len, const char *passphrase,
                          size_t passphrase_len, uint8_t pmk[32]);
void rtw89_glue_leave(void);

enum rtw89_glue_link_state {
    RTW89_GLUE_LINK_DOWN,           /* not trying to be on a network */
    RTW89_GLUE_LINK_JOINING,        /* authenticating or associating */
    RTW89_GLUE_LINK_ASSOCIATED,     /* associated, keys not installed yet */
    RTW89_GLUE_LINK_CONNECTED,      /* associated and able to pass data */
};

/* A transmission rate. kbps is 0 when it is not known. */
struct rtw89_glue_rate {
    uint32_t kbps;
    uint8_t  mode;                  /* 0: 802.11a/b/g, 1: 802.11n, 2: 802.11ac, 3: 802.11ax */
    uint8_t  mcs;                   /* the rate's index within its mode and stream count */
    uint8_t  nss;                   /* spatial streams */
    uint8_t  width;                 /* MHz */
};

struct rtw89_glue_link {
    enum rtw89_glue_link_state state;
    uint8_t  bssid[6];
    char     ssid[33];
    uint16_t freq;
    uint16_t aid;
    uint8_t  width;                 /* channel width in MHz */
    uint16_t center_freq;           /* centre of the whole channel, MHz */
    uint8_t  mode;                  /* 0: 802.11a/b/g, 1: 802.11n, 2: 802.11ac, 3: 802.11ax */
    uint8_t  nss;                   /* spatial streams */
    int8_t   signal;                /* dBm, of the access point's beacons; 0 if unknown */
    bool     authorized;            /* keys in place (or none needed): data flows */
    int      last_error;            /* 0, -errno, -1000-status or -2000-reason */
    uint32_t eapol_rx;

    /* data frames since the driver was probed */
    uint32_t tx_frames;
    uint32_t tx_dropped;
    uint32_t rx_frames;
    uint32_t rx_dropped;            /* all reasons, including the ones below */
    uint32_t rx_undecrypted;        /* encrypted frames the chip did not decrypt */
    uint32_t rx_replay;             /* frames with a packet number already used */
    uint32_t rx_dup;                /* retransmissions of frames already received */
    uint32_t rx_reorder_timeout;    /* frames passed on after waiting in vain for an earlier one */

    /* receive timing, by the chip's own timestamps */
    uint32_t rx_late;               /* frames handed over more than 5 ms after arriving */
    uint32_t rx_late_irq;           /* ... although the chip interrupted when they arrived */
    uint32_t rx_late_max_ms;
    uint32_t rx_ppdu_flushed;       /* frames passed on without their PPDU status report */
    bool     ppdu_flush;            /* see rtw89_glue_set_ppdu_flush() */
    bool     ax;                    /* see rtw89_glue_set_ax() */

    /* aggregation (BlockAck sessions): one bit per TID, in each direction */
    uint16_t tx_ba;
    uint16_t rx_ba;

    /* while connected: what the firmware sends at, and how the AP's last
     * frame to this station was sent */
    struct rtw89_glue_rate tx_rate;
    struct rtw89_glue_rate rx_rate;

    /* keeping the connection */
    uint32_t beacons;               /* the AP's beacons looked at */
    uint32_t beacon_losses;         /* times the driver reported them missing */
    uint32_t beacon_updates;        /* times a beacon changed the link's parameters */
    uint32_t probe_acks;            /* times the AP acknowledged the probe that follows a loss */
    bool     rejoin;                /* see rtw89_glue_set_rejoin() */
    bool     rejoining;             /* the connection was lost; trying to get it back */
    uint32_t rejoin_tries;          /* attempts so far */
    uint32_t rejoins;               /* times it came back since the driver was probed */

    /* scans started while associated, and the time they took altogether */
    uint32_t scans_connected;
    uint32_t scan_ms_connected;
    uint32_t last_scan_ms;          /* the latest scan, associated or not */
    uint32_t last_scan_channels;    /* ... and how many channels it covered */

    /* TX power: the country whose tables are in force ("00": worldwide), and
     * the limit they set on this channel at 20 MHz for 802.11n/ax, in 0.5 dB
     * steps, sending on one antenna and on both (each) */
    char     country[3];
    int8_t   txpwr_limit[2];
};

void rtw89_glue_link(struct rtw89_glue_link *link);

/*
 * The driver holds each received data frame until the chip's status report
 * for the same transmission arrives. When the report does not come, the frame
 * would wait for the next reception (often the next beacon, 100 ms later);
 * with this on (the default) it is passed on after 1 ms instead. Off is for
 * comparing.
 */
void rtw89_glue_set_ppdu_flush(bool on);

/*
 * A network that has been joined is joined again when the connection is lost
 * (the AP went away, changed channel, or sent us off), with growing pauses
 * between attempts, until rtw89_glue_leave(), rtw89_glue_down() or another
 * join. On by default; off is for testing.
 */
void rtw89_glue_set_rejoin(bool on);

/*
 * For testing on the real card. rtw89_glue_probe_ap() acts as if the driver
 * had reported missed beacons: the AP is probed, and the connection given up
 * if it does not acknowledge. rtw89_glue_drop() acts as if the connection
 * had been lost, which rtw89_glue_set_rejoin() then mends.
 */
void rtw89_glue_probe_ap(void);
void rtw89_glue_drop(void);

/*
 * Whether the next join may use 802.11ax where the network offers it (the
 * default) or stops at 802.11ac/n: for comparing, and for access points that
 * do not get on with this card's 802.11ax.
 */
void rtw89_glue_set_ax(bool on);

/*
 * Transmit one Ethernet frame (destination, source, type, payload; no FCS) of
 * @len bytes. rtw89_glue_tx_alloc() returns a handle and, in *@frame, the
 * buffer to fill in; rtw89_glue_tx() queues it for the access point, or
 * rtw89_glue_tx_cancel() gives it back. Any thread that may sleep, between
 * probe and remove. rtw89_glue_tx() returns 0, -ENETDOWN while not connected,
 * or -ENOBUFS when too many frames are waiting.
 */
void *rtw89_glue_tx_alloc(size_t len, uint8_t **frame);
int rtw89_glue_tx(void *handle);
void rtw89_glue_tx_cancel(void *handle);

/*
 * How many more frames may be handed to rtw89_glue_tx() right now. When this
 * returns zero, stop sending until the platform's tx_wake() is called. While
 * no network is joined there is always room (and the frames are dropped).
 */
unsigned int rtw89_glue_tx_room(void);

#ifdef __cplusplus
}
#endif

#endif /* _RTW89_GLUE_H */
