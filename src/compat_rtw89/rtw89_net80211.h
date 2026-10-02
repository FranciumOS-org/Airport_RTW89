/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stand-in for the parts of Linux cfg80211/mac80211 that rtw89 calls.
 *
 * The driver is built against the unmodified upstream net/cfg80211.h and
 * net/mac80211.h, so every struct it sees is the real one. What is missing on
 * macOS is the code behind those headers. rtw89_cfg80211.c and
 * rtw89_mac80211.c provide the small subset the driver needs and forward
 * everything that belongs to the 802.11 stack proper (received frames, TX
 * status, scan results, link events) to the IOKit layer through
 * struct rtw89_m80211_glue_ops.
 *
 * This header is for those two files and for the IOKit layer. Driver files
 * never include it.
 */
#ifndef _RTW89_NET80211_H
#define _RTW89_NET80211_H

#include "rtw89_compat.h"

/* ------------------------------------------------------------------ */
/*  cfg80211 side: what Linux keeps in cfg80211_registered_device       */
/* ------------------------------------------------------------------ */

struct rtw89_cfg80211_rdev {
    void *alloc_base;                   /* what kmalloc returned; rdev is aligned inside */
    const struct cfg80211_ops *ops;

    struct workqueue_struct *wq;        /* runs wiphy_work and reg_work */
    spinlock_t wiphy_work_lock;
    struct list_head wiphy_work_list;   /* pending struct wiphy_work, FIFO */
    struct work_struct wiphy_work;      /* runner: takes wiphy->mtx, runs one work */

    struct regulatory_request reg_request;
    struct work_struct reg_work;        /* delivers reg_request to wiphy->reg_notifier */

    /* Must be last: wiphy->priv[] (the mac80211 state) follows it. */
    struct wiphy wiphy __aligned(NETDEV_ALIGN);
};

static inline struct rtw89_cfg80211_rdev *wiphy_to_rdev(struct wiphy *wiphy)
{
    return container_of(wiphy, struct rtw89_cfg80211_rdev, wiphy);
}

/* ------------------------------------------------------------------ */
/*  mac80211 side: what Linux keeps in ieee80211_local / sdata / sta    */
/* ------------------------------------------------------------------ */

enum rtw89_m80211_link_event {
    RTW89_M80211_CONNECTION_LOSS,       /* AP is gone: disconnect */
    RTW89_M80211_BEACON_LOSS,           /* beacons missed: probe the AP */
    RTW89_M80211_CQM_RSSI_LOW,
    RTW89_M80211_CQM_RSSI_HIGH,
};

/*
 * Upcalls into the IOKit layer. All optional; a NULL hook drops the event
 * (and frees the skb where one is passed). skb ownership moves to the hook.
 */
struct rtw89_m80211_glue_ops {
    /* A received 802.11 frame; IEEE80211_SKB_RXCB(skb) is filled in. */
    void (*rx)(void *ctx, struct ieee80211_sta *sta, struct sk_buff *skb);
    /* A transmitted frame is done; IEEE80211_SKB_CB(skb)->flags has TX_STAT_ACK. */
    void (*tx_status)(void *ctx, struct sk_buff *skb);
    void (*scan_done)(void *ctx, bool aborted);
    /* The driver took a frame off @txq (ieee80211_tx_dequeue()). */
    void (*tx_dequeued)(void *ctx, struct ieee80211_txq *txq);
    /* The driver stopped (true) or woke (false) all TX queues. */
    void (*queues_stopped)(void *ctx, bool stopped);
    void (*link_event)(void *ctx, struct ieee80211_vif *vif,
                       enum rtw89_m80211_link_event event, s32 rssi);
    /* The driver wants a TX BlockAck session set up / torn down for @tid. */
    int (*start_tx_ba)(void *ctx, struct ieee80211_sta *sta, u16 tid, u16 timeout);
    int (*stop_tx_ba)(void *ctx, struct ieee80211_sta *sta, u16 tid);
    /* Firmware recovery asked for a full restart (ieee80211_restart_hw). */
    void (*restart)(void *ctx);
};

struct rtw89_m80211_txq {
    struct list_head schedule_entry;    /* on local->active_txqs[ac] while scheduled */
    u16 schedule_round;
    bool dead;                          /* being freed: takes no more frames */
    bool stopped;                       /* hands out no frames (BlockAck setup) */
    struct sk_buff_head frames;
    unsigned long byte_cnt;
    struct ieee80211_txq txq;           /* must be last: drv_priv[] follows */
};

struct rtw89_m80211_vif {
    struct list_head list;              /* on local->vifs */
    struct rtw89_m80211_local *local;
    bool in_driver;                     /* add_interface succeeded */
    u8 bssid[ETH_ALEN];                 /* what bss_conf.bssid points at (station) */
    struct ieee80211_vif vif;           /* must be last: drv_priv[] follows */
};

struct rtw89_m80211_sta {
    struct list_head list;              /* on local->stas */
    struct rtw89_m80211_vif *mvif;
    bool uploaded;                      /* the driver knows about this station */
    struct ieee80211_sta sta;           /* must be last: drv_priv[] follows */
};

struct rtw89_m80211_key {
    struct list_head list;              /* on local->keys */
    struct ieee80211_vif *vif;
    struct ieee80211_sta *sta;          /* NULL for group keys */
    struct ieee80211_key_conf *conf;
};

struct rtw89_m80211_local {
    /* First, so wiphy_priv(wiphy) is both the local and the hw. */
    struct ieee80211_hw hw;
    const struct ieee80211_ops *ops;
    bool registered;

    spinlock_t lock;                    /* vifs, stas, keys */
    struct list_head vifs;
    struct list_head stas;
    struct list_head keys;
    struct list_head dead;              /* struct rtw89_m80211_dead: freed after a grace period */

    spinlock_t active_txq_lock[IEEE80211_NUM_ACS];
    struct list_head active_txqs[IEEE80211_NUM_ACS];
    u16 schedule_round[IEEE80211_NUM_ACS];
    bool queues_stopped;
    struct work_struct wake_txqs_work;

    struct cfg80211_chan_def dflt_chandef;  /* for the emulated chanctx ops */

    const struct rtw89_m80211_glue_ops *glue;
    void *glue_ctx;
};

static inline struct rtw89_m80211_local *hw_to_local(struct ieee80211_hw *hw)
{
    return container_of(hw, struct rtw89_m80211_local, hw);
}
static inline struct rtw89_m80211_txq *to_mtxq(struct ieee80211_txq *txq)
{
    return container_of(txq, struct rtw89_m80211_txq, txq);
}
static inline struct rtw89_m80211_vif *to_mvif(struct ieee80211_vif *vif)
{
    return container_of(vif, struct rtw89_m80211_vif, vif);
}
static inline struct rtw89_m80211_sta *to_msta(struct ieee80211_sta *sta)
{
    return container_of(sta, struct rtw89_m80211_sta, sta);
}

/* ------------------------------------------------------------------ */
/*  API for the IOKit layer                                             */
/* ------------------------------------------------------------------ */

void rtw89_m80211_set_glue(struct ieee80211_hw *hw,
                           const struct rtw89_m80211_glue_ops *ops, void *ctx);

/*
 * Object lifetime only: these allocate the mac80211 object plus the driver's
 * private area and put it on the lists the iterators walk. Calling the driver
 * (add_interface, sta_state, ...) is up to the caller.
 */
struct ieee80211_vif *rtw89_m80211_vif_alloc(struct ieee80211_hw *hw,
                                             enum nl80211_iftype type, const u8 *addr);
void rtw89_m80211_vif_free(struct ieee80211_vif *vif);
void rtw89_m80211_vif_set_in_driver(struct ieee80211_vif *vif, bool in_driver);

struct ieee80211_sta *rtw89_m80211_sta_alloc(struct ieee80211_vif *vif, const u8 *addr);
void rtw89_m80211_sta_free(struct ieee80211_sta *sta);
void rtw89_m80211_sta_set_uploaded(struct ieee80211_sta *sta, bool uploaded);

/* Drop the frames queued for @sta. */
void rtw89_m80211_sta_purge_txqs(struct ieee80211_sta *sta);

/* Keep @txq from handing frames to the driver; returns how many are waiting on
 * it. rtw89_m80211_txq_start() lets them go again. */
unsigned int rtw89_m80211_txq_stop(struct ieee80211_txq *txq);
void rtw89_m80211_txq_start(struct ieee80211_hw *hw, struct ieee80211_txq *txq);

/* kfree() @ptr once no driver thread can still be using it; see rtw89_mac80211.c.
 * rtw89_m80211_reap(hw, true) frees everything now: only with the driver stopped. */
void rtw89_m80211_free_later(struct ieee80211_hw *hw, void *ptr, void (*release)(void *ptr));
void rtw89_m80211_reap(struct ieee80211_hw *hw, bool all);

int rtw89_m80211_key_add(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                         struct ieee80211_sta *sta, struct ieee80211_key_conf *conf);
void rtw89_m80211_key_del(struct ieee80211_hw *hw, struct ieee80211_key_conf *conf);

/* ------------------------------------------------------------------ */
/*  Station MLME (rtw89_mlme.c)                                         */
/* ------------------------------------------------------------------ */

/* A network to join, as heard in a beacon or probe response. */
struct rtw89_mlme_bss {
    u8 bssid[ETH_ALEN];
    u8 ssid[IEEE80211_MAX_SSID_LEN];
    u8 ssid_len;
    u16 freq;                   /* MHz */
    u16 capability;
    u16 beacon_int;
    const u8 *ies;
    size_t ies_len;
};

enum rtw89_mlme_state {
    RTW89_MLME_IDLE,
    RTW89_MLME_AUTHENTICATING,
    RTW89_MLME_ASSOCIATING,
    RTW89_MLME_ASSOCIATED,      /* associated; keys not installed yet (RSN) */
    RTW89_MLME_CONNECTED,       /* associated and authorised to pass data */
};

struct rtw89_mlme_status {
    enum rtw89_mlme_state state;
    u8 bssid[ETH_ALEN];
    u8 ssid[IEEE80211_MAX_SSID_LEN + 1];
    u16 freq;
    u16 aid;
    int last_error;             /* errno, or -(802.11 status/reason code) - 1000 */
    u32 eapol_rx;               /* EAPOL frames received from the AP */
    u16 tx_ba, rx_ba;           /* TID bits: BlockAck sessions in each direction */
    u8 width;                   /* MHz */
    u16 center_freq;            /* MHz, of the whole channel */
    u8 mode;                    /* 0: 802.11a/b/g, 1: n, 2: ac */
    u8 nss;                     /* spatial streams towards the AP */
};

/* All of these are called with the wiphy mutex held, except rtw89_mlme_rx(). */
/* @notify is called (under the wiphy mutex) whenever the state changes. */
void rtw89_mlme_start(struct ieee80211_hw *hw, struct ieee80211_vif *vif, void (*notify)(void));
void rtw89_mlme_stop(void);
/* @pmk: the 32-byte pairwise master key for a WPA2-PSK network, else NULL. */
int rtw89_mlme_connect(const struct rtw89_mlme_bss *bss, const u8 *pmk);
/* For tests: see every management frame the MLME transmits. */
void rtw89_mlme_set_tx_tap(void (*tap)(const u8 *frame, size_t len));
void rtw89_mlme_disconnect(u16 reason);
void rtw89_mlme_get_status(struct rtw89_mlme_status *status);
/* A received management frame that is not a beacon or probe response; takes
 * the skb. Any context: the frame is queued and handled under the wiphy mutex. */
void rtw89_mlme_rx(struct sk_buff *skb);
/* An EAPOL frame from the AP (without Ethernet header), from the data path. */
void rtw89_mlme_rx_eapol(const u8 *eapol, size_t len);
/* The driver reports that the AP is gone. Any context. */
void rtw89_mlme_connection_lost(void);
/* The driver wants to start or stop sending aggregates to @sta on @tid. Any
 * context; 0 if the request was taken, else what
 * ieee80211_start_tx_ba_session() would return. */
int rtw89_mlme_tx_ba_request(struct ieee80211_sta *sta, u16 tid, bool start);

/* ------------------------------------------------------------------ */
/*  Data path (rtw89_data.c)                                            */
/* ------------------------------------------------------------------ */

struct rtw89_data_stats {
    u32 tx_frames;
    u32 tx_dropped;
    u32 rx_frames;              /* handed to the network stack */
    u32 rx_dropped;
    u32 rx_dup;                 /* retries of a frame already seen */
    u32 rx_replay;              /* packet number did not advance */
    u32 rx_undecrypted;         /* protected, but not decrypted by the hardware */
    u32 rx_unprotected;         /* in the clear on an encrypted network */
    u32 rx_fragments;           /* fragmented frames, not supported */
    u32 rx_reorder_timeout;     /* frames released because one before them never came */
};

/* @deliver gets each received Ethernet frame; called from the receive thread.
 * @tx_wake is called when rtw89_data_tx_room() is no longer zero. */
void rtw89_data_init(struct ieee80211_hw *hw, void (*deliver)(const u8 *frame, size_t len),
                     void (*tx_wake)(void));
void rtw89_data_exit(void);

/* For the MLME, with the wiphy mutex held. Between attach and detach, frames
 * from @bssid are handled and the key handshake can be sent; everything else
 * waits for rtw89_data_authorize(). */
void rtw89_data_attach(struct ieee80211_vif *vif, struct ieee80211_sta *sta,
                       const u8 *bssid, enum nl80211_band band, bool protect);
void rtw89_data_detach(void);
void rtw89_data_authorize(void);
void rtw89_data_set_tx_key(struct ieee80211_key_conf *key);
void rtw89_data_set_rx_key(int keyidx, bool valid, u64 rsc);
/* BlockAck sessions. Receive: reorder what arrives on @tid. Transmit: hold the
 * TID's frames while the session is negotiated, see rtw89_data.c. */
int rtw89_data_rx_ba_start(u8 tid, u16 ssn, u16 buf_size);
void rtw89_data_rx_ba_stop(u8 tid);
int rtw89_data_tx_ba_prepare(u8 tid, u16 *ssn);
void rtw89_data_tx_ba_resume(u8 tid);

/* Transmit an Ethernet frame: allocate, fill skb->data, send. Any thread. */
struct sk_buff *rtw89_data_tx_alloc(size_t len);
int rtw89_data_tx(struct sk_buff *skb);
unsigned int rtw89_data_tx_room(void);
void rtw89_data_tx_dequeued(struct ieee80211_txq *txq);
/* A received 802.11 data frame (FCS on); takes the skb. */
void rtw89_data_rx(struct sk_buff *skb);
/* A received BlockAck request (FCS on); takes the skb. */
void rtw89_data_rx_bar(struct sk_buff *skb);
void rtw89_data_get_stats(struct rtw89_data_stats *stats);
/* For tests: see every data frame as it is queued for the driver. */
void rtw89_data_set_tx_tap(void (*tap)(const u8 *frame, size_t len));

/*
 * Queue a frame on @txq and wake the driver. The caller has filled
 * IEEE80211_SKB_CB(skb) (control.vif, flags, band) and skb->priority.
 */
void rtw89_m80211_tx(struct ieee80211_hw *hw, struct ieee80211_txq *txq,
                     struct sk_buff *skb);

#endif /* _RTW89_NET80211_H */
