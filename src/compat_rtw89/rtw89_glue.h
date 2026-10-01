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
};

/* Networks heard since the last rtw89_glue_scan() (beacons and probe
 * responses). Returns how many were copied to @out. */
unsigned int rtw89_glue_scan_results(struct rtw89_glue_bss *out, unsigned int max);

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
void rtw89_glue_leave(void);

enum rtw89_glue_link_state {
    RTW89_GLUE_LINK_DOWN,           /* not trying to be on a network */
    RTW89_GLUE_LINK_JOINING,        /* authenticating or associating */
    RTW89_GLUE_LINK_ASSOCIATED,     /* associated, keys not installed yet */
    RTW89_GLUE_LINK_CONNECTED,      /* associated and able to pass data */
};

struct rtw89_glue_link {
    enum rtw89_glue_link_state state;
    uint8_t  bssid[6];
    char     ssid[33];
    uint16_t freq;
    uint16_t aid;
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

    /* aggregation (BlockAck sessions): one bit per TID, in each direction */
    uint16_t tx_ba;
    uint16_t rx_ba;
};

void rtw89_glue_link(struct rtw89_glue_link *link);

/*
 * The driver holds each received data frame until the chip's status report
 * for the same transmission arrives. When the report does not come, the frame
 * would wait for the next reception (often the next beacon, 100 ms later);
 * with this on (the default) it is passed on after 2 ms instead. Off is for
 * comparing.
 */
void rtw89_glue_set_ppdu_flush(bool on);

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
