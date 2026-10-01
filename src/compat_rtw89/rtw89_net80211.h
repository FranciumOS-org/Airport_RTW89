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
};

/* All of these are called with the wiphy mutex held, except rtw89_mlme_rx(). */
void rtw89_mlme_start(struct ieee80211_hw *hw, struct ieee80211_vif *vif);
void rtw89_mlme_stop(void);
int rtw89_mlme_connect(const struct rtw89_mlme_bss *bss);
void rtw89_mlme_disconnect(u16 reason);
void rtw89_mlme_get_status(struct rtw89_mlme_status *status);
/* A received frame that is not a beacon or probe response; takes the skb.
 * Any context: the frame is queued and handled under the wiphy mutex. */
void rtw89_mlme_rx(struct sk_buff *skb);

/*
 * Queue a frame on @txq and wake the driver. The caller has filled
 * IEEE80211_SKB_CB(skb) (control.vif, flags, band) and skb->priority.
 */
void rtw89_m80211_tx(struct ieee80211_hw *hw, struct ieee80211_txq *txq,
                     struct sk_buff *skb);

#endif /* _RTW89_NET80211_H */
