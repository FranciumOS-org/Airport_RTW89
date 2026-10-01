/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * mac80211 API shims for rtw88 macOS port.
 *
 * This replaces the Linux mac80211 subsystem.  On macOS the driver manages
 * its own 802.11 state machine; these structures keep the driver C code
 * compiling without modification while the IOKit layer calls the ops
 * directly.
 */
#ifndef _RTW88_COMPAT_MAC80211_H
#define _RTW88_COMPAT_MAC80211_H

#include "../linux/types.h"
#include "../linux/skbuff.h"
#include "../linux/spinlock.h"
#include "../linux/mutex.h"
#include "../linux/workqueue.h"
#include "../linux/if_ether.h"
#include "../linux/etherdevice.h"
#include "../linux/slab.h"

struct regulatory_request;
struct ieee80211_link_sta;
struct ieee80211_ampdu_params;
struct ieee80211_channel_switch;
struct ieee80211_low_level_stats;
struct cfg80211_wowlan;
struct cfg80211_sar_specs;
struct ieee80211_tx_queue_params;
struct cfg80211_bitrate_mask;

/* ------------------------------------------------------------------ */
/*  802.11 frame control / header                                       */
/* ------------------------------------------------------------------ */

#define IEEE80211_FCTL_VERS     0x0003
#define IEEE80211_FCTL_FTYPE    0x000c
#define IEEE80211_FCTL_STYPE    0x00f0
#define IEEE80211_FCTL_TODS     0x0100
#define IEEE80211_FCTL_FROMDS   0x0200
#define IEEE80211_FCTL_MOREFRAGS 0x0400
#define IEEE80211_FCTL_RETRY    0x0800
#define IEEE80211_FCTL_PM       0x1000
#define IEEE80211_FCTL_MOREDATA 0x2000
#define IEEE80211_FCTL_PROTECTED 0x4000
#define IEEE80211_FCTL_ORDER    0x8000

#define IEEE80211_FTYPE_MGMT    0x0000
#define IEEE80211_FTYPE_CTL     0x0004
#define IEEE80211_FTYPE_DATA    0x0008

#define IEEE80211_STYPE_ASSOC_REQ   0x0000
#define IEEE80211_STYPE_ASSOC_RESP  0x0010
#define IEEE80211_STYPE_PROBE_REQ   0x0040
#define IEEE80211_STYPE_PROBE_RESP  0x0050
#define IEEE80211_STYPE_BEACON      0x0080
#define IEEE80211_STYPE_AUTH        0x00B0
#define IEEE80211_STYPE_DEAUTH      0x00C0
#define IEEE80211_STYPE_ACTION      0x00D0
#define IEEE80211_STYPE_DISASSOC    0x00A0
/* Data subtype: QoS Data (carries a 2-byte QoS Control field; required for
 * A-MPDU / BlockAck, which are strictly per-TID). Combined with FTYPE_DATA. */
#define IEEE80211_STYPE_QOS_DATA    0x0080
#define IEEE80211_QOS_CTL_TID_MASK  0x000f

struct ieee80211_hdr {
    __le16 frame_control;
    __le16 duration_id;
    u8 addr1[ETH_ALEN];
    u8 addr2[ETH_ALEN];
    u8 addr3[ETH_ALEN];
    __le16 seq_ctrl;
    u8 addr4[ETH_ALEN];
} __packed;

/* Management frame body (used for beacon/probe_resp scanning) */
struct ieee80211_mgmt {
    __le16 frame_control;
    __le16 duration;
    u8 da[ETH_ALEN];
    u8 sa[ETH_ALEN];
    u8 bssid[ETH_ALEN];
    __le16 seq_ctrl;
    union {
        struct {
            __le64 timestamp;
            __le16 beacon_int;
            __le16 capab_info;
            u8 variable[0];
        } __packed beacon;
        struct {
            __le64 timestamp;
            __le16 beacon_int;
            __le16 capab_info;
            u8 variable[0];
        } __packed probe_resp;
        struct {
            u8 variable[0];
        } __packed action;
    } u;
} __packed;

struct ieee80211_hdr_3addr {
    __le16 frame_control;
    __le16 duration_id;
    u8 addr1[ETH_ALEN];
    u8 addr2[ETH_ALEN];
    u8 addr3[ETH_ALEN];
    __le16 seq_ctrl;
} __packed;

static inline int ieee80211_is_mgmt(__le16 fc)
{
    return (fc & cpu_to_le16(0x000c)) == cpu_to_le16(IEEE80211_FTYPE_MGMT);
}
static inline int ieee80211_is_data(__le16 fc)
{
    return (fc & cpu_to_le16(0x000c)) == cpu_to_le16(IEEE80211_FTYPE_DATA);
}
static inline int ieee80211_is_ctl(__le16 fc)
{
    return (fc & cpu_to_le16(0x000c)) == cpu_to_le16(IEEE80211_FTYPE_CTL);
}
static inline int ieee80211_has_tods(__le16 fc)
{
    return !!(fc & cpu_to_le16(IEEE80211_FCTL_TODS));
}
static inline int ieee80211_has_fromds(__le16 fc)
{
    return !!(fc & cpu_to_le16(IEEE80211_FCTL_FROMDS));
}
static inline int ieee80211_has_protected(__le16 fc)
{
    return !!(fc & cpu_to_le16(IEEE80211_FCTL_PROTECTED));
}
static inline int ieee80211_has_moredata(__le16 fc)
{
    return !!(fc & cpu_to_le16(IEEE80211_FCTL_MOREDATA));
}
static inline int ieee80211_is_beacon(__le16 fc)
{
    return (fc & cpu_to_le16(IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE)) ==
           cpu_to_le16(IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_BEACON);
}
static inline int ieee80211_is_probe_resp(__le16 fc)
{
    return (fc & cpu_to_le16(IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE)) ==
           cpu_to_le16(IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_PROBE_RESP);
}
static inline int ieee80211_is_action(__le16 fc)
{
    return (fc & cpu_to_le16(IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE)) ==
           cpu_to_le16(IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_ACTION);
}
static inline int ieee80211_is_nullfunc(__le16 fc)
{
    return (le16_to_cpu(fc) & (0x00fc | 0x000c)) == (0x0048 | IEEE80211_FTYPE_DATA);
}
static inline int ieee80211_is_data_qos(__le16 fc)
{
    return ((le16_to_cpu(fc) & (0x000c | 0x0080)) == (IEEE80211_FTYPE_DATA | 0x0080));
}

static inline u16 ieee80211_get_hdrlen_from_skb(const struct sk_buff *skb)
{
    const struct ieee80211_hdr *hdr = (const struct ieee80211_hdr *)skb->data;
    u16 fc = le16_to_cpu(hdr->frame_control);
    bool has_a4 = (fc & (IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS)) ==
                  (IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS);
    bool is_qos = (fc & 0x0080) && ((fc & 0x000c) == IEEE80211_FTYPE_DATA);
    u16 len = 24;
    if (has_a4) len += 6;
    if (is_qos)  len += 2;
    return len;
}

/* ------------------------------------------------------------------ */
/*  Bands and channels                                                  */
/* ------------------------------------------------------------------ */

enum nl80211_band {
    NL80211_BAND_2GHZ = 0,
    NL80211_BAND_5GHZ = 1,
    NL80211_BAND_60GHZ = 2,
    NL80211_NUM_BANDS
};

enum nl80211_channel_type {
    NL80211_CHAN_NO_HT,
    NL80211_CHAN_HT20,
    NL80211_CHAN_HT40MINUS,
    NL80211_CHAN_HT40PLUS,
};

#define IEEE80211_CHAN_DISABLED     (1 << 0)
#define IEEE80211_CHAN_NO_IR        (1 << 1)
#define IEEE80211_CHAN_RADAR        (1 << 7)
#define IEEE80211_CHAN_NO_HT40PLUS  (1 << 9)
#define IEEE80211_CHAN_NO_HT40MINUS (1 << 10)
#define IEEE80211_CHAN_NO_OFDM      (1 << 6)

struct ieee80211_channel {
    enum nl80211_band band;
    u16 center_freq;
    u16 hw_value;
    u32 flags;
    int max_power;
    int max_reg_power;
};

struct ieee80211_rate {
    u32 bitrate;   /* in units of 100 Kbps */
    u16 hw_value;
    u16 hw_value_short;
    u32 flags;
};

#define IEEE80211_RATE_SHORT_PREAMBLE 0x1

struct ieee80211_sta_ht_cap {
    u16 cap;
    bool ht_supported;
    u8 ampdu_factor;
    u8 ampdu_density;
    struct { u8 rx_mask[10]; u16 rx_highest; u32 tx_params; } mcs;
};

struct ieee80211_sta_vht_cap {
    bool vht_supported;
    u32 cap;
    struct { __le32 rx_mcs_map; __le16 rx_highest;
             __le32 tx_mcs_map; __le16 tx_highest; } vht_mcs;
};

struct ieee80211_supported_band {
    struct ieee80211_channel *channels;
    int n_channels;
    struct ieee80211_rate *bitrates;
    int n_bitrates;
    struct ieee80211_sta_ht_cap  ht_cap;
    struct ieee80211_sta_vht_cap vht_cap;
    enum nl80211_band band;
};

/* ------------------------------------------------------------------ */
/*  cfg80211 channel definitions                                        */
/* ------------------------------------------------------------------ */

enum nl80211_chan_width {
    NL80211_CHAN_WIDTH_20_NOHT = 0,
    NL80211_CHAN_WIDTH_20      = 1,
    NL80211_CHAN_WIDTH_40      = 2,
    NL80211_CHAN_WIDTH_80      = 3,
    NL80211_CHAN_WIDTH_80P80   = 4,
    NL80211_CHAN_WIDTH_160     = 5,
    NL80211_CHAN_WIDTH_5       = 6,
    NL80211_CHAN_WIDTH_10      = 7,
};

struct cfg80211_chan_def {
    struct ieee80211_channel *chan;
    enum nl80211_chan_width    width;
    u32                       center_freq1;
    u32                       center_freq2;
};

/* ------------------------------------------------------------------ */
/*  wiphy (cfg80211 wireless device — simplified shim)                  */
/* ------------------------------------------------------------------ */

#define WIPHY_FLAG_SUPPORTS_TDLS         (1 << 0)
#define WIPHY_FLAG_TDLS_EXTERNAL_SETUP   (1 << 1)

#define NL80211_FEATURE_SCAN_RANDOM_MAC_ADDR  (1 << 0)

enum nl80211_ext_feature_index {
    NL80211_EXT_FEATURE_CAN_REPLACE_PTK0 = 0,
    NL80211_EXT_FEATURE_SCAN_RANDOM_SN   = 1,
    NL80211_EXT_FEATURE_SET_SCAN_DWELL   = 2,
    NUM_NL80211_EXT_FEATURES,
};

struct wiphy {
    void *_dev;  /* rtw_dev pointer — stored at offset 0 so wiphy_to_ieee80211_hw
                  * can return (ieee80211_hw*)wiphy and hw->priv (offset 0) = rtwdev */
    struct ieee80211_supported_band *bands[NL80211_NUM_BANDS];
    u32   interface_modes;
    u32   flags;
    u32   features;
    u8    available_antennas_tx;
    u8    available_antennas_rx;
    u16   max_scan_ssids;
    u16   max_scan_ie_len;
    u16   max_sched_scan_ssids;
    u32   rts_threshold;
    const struct ieee80211_iface_combination *iface_combinations;
    u32   n_iface_combinations;
    const void *wowlan;
    const void *sar_capa;
    u8    perm_addr[ETH_ALEN];
    u32   regulatory_flags;
    void (*reg_notifier)(struct wiphy *wiphy, struct regulatory_request *request);
    /* ext features bitmap — one bit per enum nl80211_ext_feature_index */
    u8    ext_features[(NUM_NL80211_EXT_FEATURES + 7) / 8];
    /* rtw89: wiphy mutex (wiphy_lock/unlock; held while wiphy_work runs) and the
     * wiphy_work runner � see src/compat_rtw89/rtw89_compat.c. Initialised by
     * rtw89_compat_wiphy_init() wherever the wiphy is allocated. */
    struct mutex mtx;
    spinlock_t wiphy_work_lock;
    struct list_head wiphy_work_list;
    struct work_struct wiphy_work_runner;
};

static inline void wiphy_ext_feature_set(struct wiphy *wiphy,
                                          enum nl80211_ext_feature_index idx)
{
    wiphy->ext_features[idx / 8] |= (u8)(1u << (idx % 8));
}

/* ------------------------------------------------------------------ */
/*  ieee80211_hw  (hardware descriptor)                                 */
/* ------------------------------------------------------------------ */

/* HW capability flags — used by ieee80211_hw_set() */
#define IEEE80211_HW_RX_INCLUDES_FCS            (1u << 0)
#define IEEE80211_HW_SIGNAL_DBM                 (1u << 1)
#define IEEE80211_HW_SPECTRUM_MGMT              (1u << 2)
#define IEEE80211_HW_REPORTS_TX_ACK_STATUS      (1u << 3)
#define IEEE80211_HW_MFP_CAPABLE                (1u << 4)
#define IEEE80211_HW_SUPPORTS_PS                (1u << 5)
#define IEEE80211_HW_SUPPORTS_DYNAMIC_PS        (1u << 6)
#define IEEE80211_HW_AMPDU_AGGREGATION          (1u << 7)
#define IEEE80211_HW_SUPPORT_FAST_XMIT          (1u << 8)
#define IEEE80211_HW_SUPPORTS_AMSDU_IN_AMPDU    (1u << 9)
#define IEEE80211_HW_HAS_RATE_CONTROL           (1u << 10)
#define IEEE80211_HW_TX_AMSDU                   (1u << 11)
#define IEEE80211_HW_SINGLE_SCAN_ON_ALL_BANDS   (1u << 12)
#define IEEE80211_HW_WANT_MONITOR_VIF           (1u << 13)
#define IEEE80211_HW_NO_AUTO_VIF                (1u << 14)
#define IEEE80211_HW_SW_CRYPTO_CONTROL          (1u << 15)

/* ieee80211_hw_set(hw, FLAG) → hw->flags |= IEEE80211_HW_FLAG */
#define ieee80211_hw_set(hw, flg)   ((hw)->flags |= IEEE80211_HW_##flg)
#define ieee80211_hw_check(hw, flg) ((hw)->flags &  IEEE80211_HW_##flg)

struct ieee80211_hw {
    void *priv;
    struct wiphy *wiphy;
    const struct ieee80211_ops *ops;
    struct ieee80211_supported_band *wiphy_bands[NL80211_NUM_BANDS];
    int  queues;
    u16  max_rates;
    u16  max_rate_tries;
    u16  extra_tx_headroom;
    u32  flags;
    u32  required_mask;
    u32  txq_data_size;
    u32  sta_data_size;
    u32  vif_data_size;

    /* hw->conf: current config flags consulted by driver */
    struct {
        u32  flags;
        int  power_level;
        u16  listen_interval;
        int  dynamic_ps_timeout;
        struct cfg80211_chan_def chandef;
    } conf;

    void *kext_hw;
};

#define IEEE80211_CONF_IDLE      (1 << 4)
#define IEEE80211_CONF_PS        (1 << 5)
#define IEEE80211_CONF_MONITOR         (1 << 6)
#define IEEE80211_CONF_CHANGE_LISTEN_INTERVAL (1 << 2)
#define IEEE80211_CONF_CHANGE_MONITOR  (1 << 3)
#define IEEE80211_CONF_CHANGE_PS       (1 << 4)
#define IEEE80211_CONF_CHANGE_POWER    (1 << 5)
#define IEEE80211_CONF_CHANGE_CHANNEL  (1 << 6)
#define IEEE80211_CONF_CHANGE_RETRY_LIMITS (1 << 7)
#define IEEE80211_CONF_CHANGE_IDLE     (1 << 8)
#define IEEE80211_CONF_CHANGE_SMPS     (1 << 9)

/* Frame filter flags */
#define FIF_ALLMULTI            (1 << 1)
#define FIF_FCSFAIL             (1 << 2)
#define FIF_PLCPFAIL            (1 << 3)
#define FIF_BCN_PRBRESP_PROMISC (1 << 4)
#define FIF_CONTROL             (1 << 5)
#define FIF_OTHER_BSS           (1 << 6)
#define FIF_PSPOLL              (1 << 7)
#define FIF_PROBE_REQ           (1 << 8)

#define SET_IEEE80211_PERM_ADDR(hw, addr) \
    memcpy((hw)->wiphy->perm_addr, (addr), ETH_ALEN)

#define SET_IEEE80211_DEV(hw, dev) \
    do { (void)(dev); } while (0)

/* ------------------------------------------------------------------ */
/*  ieee80211_vif  (virtual interface)                                  */
/* ------------------------------------------------------------------ */

enum nl80211_iftype {
    NL80211_IFTYPE_UNSPECIFIED,
    NL80211_IFTYPE_ADHOC,
    NL80211_IFTYPE_STATION,
    NL80211_IFTYPE_AP,
    NL80211_IFTYPE_AP_VLAN,
    NL80211_IFTYPE_WDS,
    NL80211_IFTYPE_MONITOR,
    NL80211_IFTYPE_MESH_POINT,
    NL80211_IFTYPE_P2P_CLIENT,
    NL80211_IFTYPE_P2P_GO,
    NL80211_IFTYPE_P2P_DEVICE,
    NL80211_IFTYPE_OCB,
    NL80211_IFTYPE_NAN,
};

struct ieee80211_bss_conf {
    const u8 *bssid;
    u8 bssid_buf[ETH_ALEN];
    bool assoc;
    bool ibss_joined;
    u16 aid;
    bool use_cts_prot;
    bool use_short_preamble;
    bool use_short_slot;
    bool enable_beacon;
    u8 dtim_period;
    u16 beacon_int;
    u16 assoc_capability;
    u64 sync_tsf;
    u32 sync_device_ts;
    u8 sync_dtim_count;
    u32 basic_rates;
    int mcast_rate[NL80211_NUM_BANDS];
    u16 ht_operation_mode;
    s32 cqm_rssi_thold;
    u32 cqm_rssi_hyst;
    s32 cqm_rssi_low;
    s32 cqm_rssi_high;
    struct ieee80211_channel *chandef_chan;
    u8 chandef_width;  /* 0=20, 1=40, 2=80, etc. */
    bool qos;
    bool hidden_ssid;
    int txpower;
    bool p2p_noa_attr;
    u8 p2p_oppps_ctwindow;
    struct {
        u8 membership[8];
        u8 position[16];
    } mu_group;
};

#define BSS_CHANGED_ASSOC       (1 << 2)
#define BSS_CHANGED_ERP_CTS_PROT (1 << 3)
#define BSS_CHANGED_ERP_PREAMBLE (1 << 4)
#define BSS_CHANGED_ERP_SLOT    (1 << 5)
#define BSS_CHANGED_HT          (1 << 6)
#define BSS_CHANGED_BASIC_RATES (1 << 7)
#define BSS_CHANGED_BEACON_INT  (1 << 8)
#define BSS_CHANGED_BSSID       (1 << 9)
#define BSS_CHANGED_BEACON      (1 << 10)
#define BSS_CHANGED_BEACON_ENABLED (1 << 11)
#define BSS_CHANGED_CQM         (1 << 12)
#define BSS_CHANGED_IBSS        (1 << 13)
#define BSS_CHANGED_ARP_FILTER  (1 << 14)
#define BSS_CHANGED_QOS         (1 << 15)
#define BSS_CHANGED_IDLE        (1 << 16)
#define BSS_CHANGED_SSID        (1 << 17)
#define BSS_CHANGED_AP_PROBE_RESP (1 << 18)
#define BSS_CHANGED_PS          (1 << 19)
#define BSS_CHANGED_TXPOWER     (1 << 20)
#define BSS_CHANGED_P2P_PS      (1 << 21)
#define BSS_CHANGED_BEACON_INFO (1 << 22)
#define BSS_CHANGED_BANDWIDTH   (1 << 23)
#define BSS_CHANGED_OCB         (1 << 25)
#define BSS_CHANGED_MU_GROUPS   (1 << 26)
#define BSS_CHANGED_KEEP_ALIVE  (1 << 28)
#define BSS_CHANGED_MCAST_RATE  (1 << 29)

/* VIF driver flags */
#define IEEE80211_VIF_BEACON_FILTER       (1 << 0)
#define IEEE80211_VIF_SUPPORTS_CQM_RSSI  (1 << 1)

struct ieee80211_vif {
    enum nl80211_iftype type;
    struct ieee80211_bss_conf bss_conf;
    /* vif->cfg mirrors bss_conf for newer kernel compat */
    struct {
        bool assoc;
        u16  aid;
        u16  ssid_len;
        u8   ssid[32];
        bool ps;
    } cfg;
    struct ieee80211_txq *txq;
    u8   addr[ETH_ALEN];
    bool p2p;
    u32  driver_flags;
    u8   drv_priv[0] __attribute__((aligned(sizeof(void *))));
};

/* ------------------------------------------------------------------ */
/*  ieee80211_sta  (station / peer)                                     */
/* ------------------------------------------------------------------ */

/* Must be defined before ieee80211_sta (used in txq array size) */
#ifndef IEEE80211_NUM_TIDS
#define IEEE80211_NUM_TIDS 16
#endif

/* RX bandwidth (used by sta->deflink.bandwidth) */
enum ieee80211_sta_rx_bandwidth {
    IEEE80211_STA_RX_BW_20  = 0,
    IEEE80211_STA_RX_BW_40  = 1,
    IEEE80211_STA_RX_BW_80  = 2,
    IEEE80211_STA_RX_BW_160 = 3,
    IEEE80211_STA_RX_BW_320 = 4,
};

struct ieee80211_sta_rates {
    struct { s8 idx; u8 count; u8 count_cts; u8 count_rts; u32 flags; } rate[4];
};

struct ieee80211_sta {
    u8   addr[ETH_ALEN];
    u16  aid;
    bool wme;
    bool mfp;
    bool tdls;
    u16  max_rc_amsdu_len;
    /* txq[0..IEEE80211_NUM_TIDS-1]: per-TID, txq[IEEE80211_NUM_TIDS]: non-QoS */
    struct ieee80211_txq *txq[IEEE80211_NUM_TIDS + 1];
    /* deflink: per-link station state (mirrors older flat fields) */
    struct {
        struct ieee80211_sta_ht_cap  ht_cap;
        struct ieee80211_sta_vht_cap vht_cap;
        u32  supp_rates[NL80211_NUM_BANDS];
        enum ieee80211_sta_rx_bandwidth bandwidth;
        struct {
            u16 max_rc_amsdu_len;
        } agg;
    } deflink;
    u8   drv_priv[0] __attribute__((aligned(sizeof(void *))));
};

/* ------------------------------------------------------------------ */
/*  TX  info                                                            */
/* ------------------------------------------------------------------ */

#define IEEE80211_TX_CTL_REQ_TX_STATUS  (1 << 0)
#define IEEE80211_TX_CTL_ASSIGN_SEQ     (1 << 1)
#define IEEE80211_TX_CTL_NO_ACK         (1 << 2)
#define IEEE80211_TX_CTL_FIRST_FRAGMENT (1 << 4)
#define IEEE80211_TX_CTL_SEND_AFTER_DTIM (1 << 5)
#define IEEE80211_TX_CTL_AMPDU          (1 << 6)
#define IEEE80211_TX_CTL_INJECTED       (1 << 7)
#define IEEE80211_TX_STAT_TX_FILTERED   (1 << 8)
#define IEEE80211_TX_STAT_ACK           (1 << 9)
#define IEEE80211_TX_STAT_AMPDU         (1 << 10)
#define IEEE80211_TX_STAT_AMPDU_NO_BACK          (1 << 11)
#define IEEE80211_TX_STAT_NOACK_TRANSMITTED      (1 << 16)
#define IEEE80211_TX_CTL_RATE_CTRL_PROBE (1 << 12)
#define IEEE80211_TX_CTL_CLEAR_PS_FILT  (1 << 13)
#define IEEE80211_TX_CTL_USE_MINRATE    (1 << 14)
#define IEEE80211_TX_CTL_DONTFRAG       (1 << 15)
#define IEEE80211_TX_CTL_HW_80211_ENCAP (1 << 30)
#define IEEE80211_TX_CTL_MCAST_MLO_FIRST_TX (1<<16)

struct ieee80211_tx_rate {
    s8  idx;
    u16 count : 5;
    u32 flags;
};

#define IEEE80211_TX_MAX_RATES  4

struct ieee80211_tx_info {
    u32 flags;
    u8  band;
    s8  tx_time_est;
    union {
        struct {
            struct ieee80211_tx_rate rates[IEEE80211_TX_MAX_RATES];
            s8  rts_cts_rate_idx;
            u8  use_rts : 1;
            u8  use_cts_prot : 1;
            u8  short_preamble : 1;
            u8  skip_table : 1;
            struct ieee80211_vif *vif;
            struct ieee80211_key_conf *hw_key;
            struct ieee80211_sta *sta;
        } control;
        struct {
            struct ieee80211_tx_rate rates[IEEE80211_TX_MAX_RATES];
            int ack_signal;
            u8  ampdu_ack_len;
            u8  ampdu_len;
            u8  antenna;
            u32 tx_time;
            bool is_valid_ack_signal;
            /* driver status area */
            u8 status_driver_data[20] __attribute__((aligned(8)));
        } status;
        struct { u8 pad[64]; } padding;
    };
};

static inline struct ieee80211_tx_info *IEEE80211_SKB_CB(struct sk_buff *skb)
{
    return (struct ieee80211_tx_info *)skb->cb;
}

/* ------------------------------------------------------------------ */
/*  TX queue                                                            */
/* ------------------------------------------------------------------ */

#define IEEE80211_AC_VO 0
#define IEEE80211_AC_VI 1
#define IEEE80211_AC_BE 2
#define IEEE80211_AC_BK 3
#define IEEE80211_NUM_ACS 4

/* enum used as parameter type: enum ieee80211_ac_numbers ac */
enum ieee80211_ac_numbers {
    IEEE80211_AC_NUMBERS_VO = 0,
    IEEE80211_AC_NUMBERS_VI = 1,
    IEEE80211_AC_NUMBERS_BE = 2,
    IEEE80211_AC_NUMBERS_BK = 3,
};

/* Sequence control mask */
#define IEEE80211_SCTL_SEQ  0xFFF0

struct ieee80211_txq {
    struct ieee80211_vif *vif;
    struct ieee80211_sta *sta;
    u8 tid;
    u8 ac;
    /* driver private area */
    u8 drv_priv[0] __attribute__((aligned(sizeof(void *))));
};

/* ------------------------------------------------------------------ */
/*  RX status                                                           */
/* ------------------------------------------------------------------ */

struct ieee80211_rx_status {
    u64  mactime;
    u32  device_timestamp;
    u32  ampdu_reference;
    u32  flag;
    u16  freq;
    u8   rate_idx;
    u8   nss;      /* number of spatial streams */
    u8   vht_nss;  /* alias for nss in older code */
    u8   rx_flags;
    u8   band;
    u8   encoding;
    u8   bw;
    s8   signal;
    u8   chains;
    s8   chain_signal[4];
    u8   antenna;
    u8   ampdu_delimiter_crc;
    bool zero_length_psdu_type;
};

#define IEEE80211_SKB_RXCB(skb) ((struct ieee80211_rx_status *)(skb)->cb)

/* RX flags */
#define RX_FLAG_DECRYPTED           (1 << 0)
#define RX_FLAG_MMIC_STRIPPED       (1 << 1)
#define RX_FLAG_IV_STRIPPED         (1 << 2)
#define RX_FLAG_FAILED_FCS_CRC      (1 << 3)
#define RX_FLAG_FAILED_PLCP_CRC     (1 << 4)
#define RX_FLAG_MACTIME_PLCP_START  (1 << 5)
#define RX_FLAG_NO_SIGNAL_VAL       (1 << 6)
#define RX_FLAG_AMPDU_DETAILS       (1 << 8)
#define RX_FLAG_PN_VALIDATED        (1 << 9)
#define RX_FLAG_DUP_VALIDATED       (1 << 10)
#define RX_FLAG_AMPDU_LAST_KNOWN    (1 << 11)
#define RX_FLAG_AMPDU_IS_LAST       (1 << 12)
#define RX_FLAG_AMPDU_DELIM_CRC_ERROR (1 << 13)
#define RX_FLAG_AMPDU_DELIM_CRC_KNOWN (1 << 14)
#define RX_FLAG_MACTIME_END         (1 << 15)
#define RX_FLAG_ONLY_MONITOR        (1 << 16)
#define RX_FLAG_SKIP_MONITOR        (1 << 17)
#define RX_FLAG_AMSDU_MORE          (1 << 18)
#define RX_FLAG_RADIOTAP_VENDOR_DATA (1 << 19)
#define RX_FLAG_MIC_STRIPPED        (1 << 20)
#define RX_FLAG_ALLOW_SAME_PN       (1 << 21)
#define RX_FLAG_ICV_STRIPPED        (1 << 22)
#define RX_FLAG_AMPDU_EOF_BIT       (1 << 23)
#define RX_FLAG_AMPDU_EOF_BIT_KNOWN (1 << 24)
#define RX_FLAG_RADIOTAP_HE         (1 << 25)
#define RX_FLAG_RADIOTAP_HE_MU      (1 << 26)
#define RX_FLAG_MACTIME_START       (1 << 29)
#define RX_FLAG_MACTIME             (1 << 27)
#define RX_FLAG_NO_PSDU             (1 << 28)

/* Rate encoding */
#define RX_ENC_LEGACY    0
#define RX_ENC_HT        1
#define RX_ENC_VHT       2
#define RX_ENC_HE        3

/* BW encoding */
#define RATE_INFO_BW_20  0
#define RATE_INFO_BW_40  1
#define RATE_INFO_BW_80  2
#define RATE_INFO_BW_160 3

/* ------------------------------------------------------------------ */
/*  Key config                                                          */
/* ------------------------------------------------------------------ */

#define WLAN_CIPHER_SUITE_WEP40         0x000FAC01
#define WLAN_CIPHER_SUITE_AES_CMAC      0x000FAC06
#define WLAN_CIPHER_SUITE_BIP_GMAC_128  0x000FAC0B
#define WLAN_CIPHER_SUITE_BIP_GMAC_256  0x000FAC0C
#define WLAN_CIPHER_SUITE_TKIP   0x000FAC02
#define WLAN_CIPHER_SUITE_CCMP   0x000FAC04
#define WLAN_CIPHER_SUITE_WEP104 0x000FAC05
#define WLAN_CIPHER_SUITE_CMAC   0x000FAC06
#define WLAN_CIPHER_SUITE_GCMP   0x000FAC08
#define WLAN_CIPHER_SUITE_GCMP_256 0x000FAC09
#define WLAN_CIPHER_SUITE_CCMP_256 0x000FAC0A
#define WLAN_CIPHER_SUITE_BIP_CMAC_256 0x000FAC0D

#define IEEE80211_MAX_KEY_SEQ_LEN 16

struct ieee80211_key_conf {
    u32  cipher;
    u8   icv_len;
    u8   iv_len;
    u8   hw_key_idx;
    s8   keyidx;
    u16  flags;
    u8   keylen;
    u8   key[32];
};

#define IEEE80211_KEY_FLAG_GENERATE_IV     (1 << 1)
#define IEEE80211_KEY_FLAG_GENERATE_MMIC   (1 << 2)
#define IEEE80211_KEY_FLAG_PAIRWISE        (1 << 3)
#define IEEE80211_KEY_FLAG_SW_MGMT_TX      (1 << 4)
#define IEEE80211_KEY_FLAG_PUT_IV_SPACE    (1 << 5)
#define IEEE80211_KEY_FLAG_RX_MGMT         (1 << 6)
#define IEEE80211_KEY_FLAG_GENERATE_IV_MGMT (1 << 7)

enum set_key_cmd { SET_KEY, DISABLE_KEY };

/* ------------------------------------------------------------------ */
/*  ieee80211_ops                                                       */
/* ------------------------------------------------------------------ */

struct ieee80211_hw;
struct ieee80211_tx_control {
    struct ieee80211_sta *sta;
};

#ifndef IEEE80211_MAX_SSID_LEN
#define IEEE80211_MAX_SSID_LEN  32
#endif

struct cfg80211_ssid_entry {
    u8  ssid[IEEE80211_MAX_SSID_LEN];
    u8  ssid_len;
};

struct cfg80211_scan_request {
    struct cfg80211_ssid_entry *ssids;
    int n_ssids;
    struct ieee80211_channel **channels;
    int n_channels;
    const u8 *ie;
    size_t    ie_len;
    u32       flags;
    bool      no_cck;
    bool      duration_mandatory;
    u16       duration;
    u8        mac_addr[ETH_ALEN];
    u8        mac_addr_mask[ETH_ALEN];
};

/* per-band scan IEs appended to probe requests */
struct ieee80211_scan_ies {
    const u8 *ies[NL80211_NUM_BANDS];
    size_t    len[NL80211_NUM_BANDS];
    const u8 *common_ies;
    size_t    common_ie_len;
};

struct ieee80211_scan_request {
    struct ieee80211_scan_ies ies;
    struct cfg80211_scan_request req;
};

#define NUM_NL80211_BANDS  NL80211_NUM_BANDS

/* Scan flags */
#define NL80211_SCAN_FLAG_LOW_PRIORITY   (1 << 0)
#define NL80211_SCAN_FLAG_RANDOM_ADDR    (1 << 2)
#define NL80211_SCAN_FLAG_RANDOM_SN      (1 << 3)

struct ieee80211_chanctx_conf {
    struct ieee80211_channel *def_chan;
    u8  rx_chains_static;
    u8  rx_chains_dynamic;
    u8  drv_priv[0] __attribute__((aligned(sizeof(void *))));
};

/* Forward declarations for types used in ieee80211_ops */
struct station_info;
struct ieee80211_prep_tx_info;

/* Reconfig type (used in ieee80211_ops::reconfig_complete) */
enum ieee80211_reconfig_type {
    IEEE80211_RECONFIG_TYPE_RESTART,
    IEEE80211_RECONFIG_TYPE_SUSPEND,
};

/* Chanctx switch mode (used in ieee80211_ops::switch_vif_chanctx) */
enum ieee80211_chanctx_switch_mode {
    CHANCTX_SWMODE_REASSIGN_VIF,
    CHANCTX_SWMODE_SWAP_CONTEXTS,
};

struct ieee80211_vif_chanctx_switch {
    struct ieee80211_vif            *vif;
    struct ieee80211_chanctx_conf   *old_ctx;
    struct ieee80211_chanctx_conf   *new_ctx;
};

struct ieee80211_ops {
    void (*tx)(struct ieee80211_hw *hw,
               struct ieee80211_tx_control *control,
               struct sk_buff *skb);
    int  (*start)(struct ieee80211_hw *hw);
    void (*stop)(struct ieee80211_hw *hw, bool suspend);
    int  (*add_interface)(struct ieee80211_hw *hw, struct ieee80211_vif *vif);
    void (*remove_interface)(struct ieee80211_hw *hw, struct ieee80211_vif *vif);
    int  (*change_interface)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                              enum nl80211_iftype type, bool p2p);
    int  (*config)(struct ieee80211_hw *hw, int radio_idx, u32 changed);
    void (*configure_filter)(struct ieee80211_hw *hw,
                              unsigned int changed_flags,
                              unsigned int *total_flags,
                              u64 multicast);
    int  (*sta_add)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                    struct ieee80211_sta *sta);
    int  (*sta_remove)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                       struct ieee80211_sta *sta);
    void (*sta_statistics)(struct ieee80211_hw *hw,
                           struct ieee80211_vif *vif,
                           struct ieee80211_sta *sta,
                           struct station_info *sinfo);
    void (*bss_info_changed)(struct ieee80211_hw *hw,
                              struct ieee80211_vif *vif,
                              struct ieee80211_bss_conf *info,
                              u64 changed);
    int  (*conf_tx)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                    u32 link_id, u16 ac,
                    const struct ieee80211_tx_queue_params *params);
    void (*wake_tx_queue)(struct ieee80211_hw *hw, struct ieee80211_txq *txq);
    int  (*set_key)(struct ieee80211_hw *hw, enum set_key_cmd cmd,
                    struct ieee80211_vif *vif, struct ieee80211_sta *sta,
                    struct ieee80211_key_conf *key);
    int  (*hw_scan)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                    struct ieee80211_scan_request *req);
    void (*cancel_hw_scan)(struct ieee80211_hw *hw, struct ieee80211_vif *vif);
    void (*sw_scan_start)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                          const u8 *mac_addr);
    void (*sw_scan_complete)(struct ieee80211_hw *hw, struct ieee80211_vif *vif);
    int  (*set_rts_threshold)(struct ieee80211_hw *hw, int radio_idx, u32 value);
    void (*link_sta_rc_update)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                               struct ieee80211_link_sta *link_sta, u32 changed);
    bool (*can_aggregate_in_amsdu)(struct ieee80211_hw *hw,
                                    struct sk_buff *head, struct sk_buff *skb);
    void (*mgd_prepare_tx)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                            struct ieee80211_prep_tx_info *info);
    int  (*start_ap)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                     struct ieee80211_bss_conf *link_conf);
    void (*stop_ap)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                    struct ieee80211_bss_conf *link_conf);
    int  (*ampdu_action)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                         struct ieee80211_ampdu_params *params);
    int  (*remain_on_channel)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                               struct ieee80211_channel *chan, int duration,
                               int type); /* enum ieee80211_roc_type */
    int  (*cancel_remain_on_channel)(struct ieee80211_hw *hw,
                                     struct ieee80211_vif *vif);
    int  (*set_bitrate_mask)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                              const struct cfg80211_bitrate_mask *mask);
    int  (*set_antenna)(struct ieee80211_hw *hw, int radio_idx, u32 tx_ant, u32 rx_ant);
    int  (*get_antenna)(struct ieee80211_hw *hw, int radio_idx, u32 *tx_ant, u32 *rx_ant);
    int  (*get_survey)(struct ieee80211_hw *hw, int idx, void *survey);
    void (*flush)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                  u32 queues, bool drop);
    void (*channel_switch)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                            struct ieee80211_channel_switch *ch_switch);
    int  (*set_tim)(struct ieee80211_hw *hw, struct ieee80211_sta *sta,
                    bool set);
    int  (*get_stats)(struct ieee80211_hw *hw,
                      struct ieee80211_low_level_stats *stats);
    int  (*suspend)(struct ieee80211_hw *hw, struct cfg80211_wowlan *wowlan);
    int  (*resume)(struct ieee80211_hw *hw);
    void (*set_wakeup)(struct ieee80211_hw *hw, bool enabled);
    int  (*set_sar_specs)(struct ieee80211_hw *hw,
                          const struct cfg80211_sar_specs *sar);
    void (*reconfig_complete)(struct ieee80211_hw *hw,
                               enum ieee80211_reconfig_type reconfig_type);
    int  (*get_station)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                        struct ieee80211_sta *sta, struct station_info *sinfo);
    /* Channel context ops — use ieee80211_emulate_* for single-channel drivers */
    int  (*add_chanctx)(struct ieee80211_hw *hw, struct ieee80211_chanctx_conf *ctx);
    void (*remove_chanctx)(struct ieee80211_hw *hw, struct ieee80211_chanctx_conf *ctx);
    void (*change_chanctx)(struct ieee80211_hw *hw, struct ieee80211_chanctx_conf *ctx,
                            u32 changed);
    int  (*switch_vif_chanctx)(struct ieee80211_hw *hw,
                                struct ieee80211_vif_chanctx_switch *vifs,
                                int n_vifs,
                                enum ieee80211_chanctx_switch_mode mode);
    int  (*assign_vif_chanctx)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                                struct ieee80211_bss_conf *link_conf,
                                struct ieee80211_chanctx_conf *ctx);
    void (*unassign_vif_chanctx)(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                                  struct ieee80211_bss_conf *link_conf,
                                  struct ieee80211_chanctx_conf *ctx);
};

/* ieee80211_link_sta — per-link station (deflink is the only link for non-MLO) */
struct ieee80211_link_sta {
    struct ieee80211_sta *sta;
    struct ieee80211_sta_ht_cap  ht_cap;
    struct ieee80211_sta_vht_cap vht_cap;
    u16  supp_rates[NUM_NL80211_BANDS];
    enum ieee80211_sta_rx_bandwidth bandwidth;
};

/* NL80211 station info bitmask values */
#define NL80211_STA_INFO_TX_BITRATE  (1 << 2)
#define NL80211_STA_INFO_RX_BITRATE  (1 << 3)
#define NL80211_STA_INFO_SIGNAL      (1 << 7)
#define NL80211_STA_INFO_SIGNAL_AVG  (1 << 10)

/* RC (rate control) changed bits */
#define IEEE80211_RC_BW_CHANGED      (1 << 0)
#define IEEE80211_RC_SMPS_CHANGED    (1 << 1)
#define IEEE80211_RC_SUPP_RATES_CHANGED (1 << 2)

/* (ieee80211_reconfig_type, ieee80211_chanctx_switch_mode, and
 *  ieee80211_vif_chanctx_switch are defined above ieee80211_ops) */

/* Chanctx emulation helpers — provided by mac80211; we stub them */
struct ieee80211_hw;
static inline int ieee80211_emulate_add_chanctx(struct ieee80211_hw *hw,
        struct ieee80211_chanctx_conf *ctx) { return 0; }
static inline void ieee80211_emulate_remove_chanctx(struct ieee80211_hw *hw,
        struct ieee80211_chanctx_conf *ctx) {}
static inline void ieee80211_emulate_change_chanctx(struct ieee80211_hw *hw,
        struct ieee80211_chanctx_conf *ctx, u32 changed) {}
static inline int ieee80211_emulate_switch_vif_chanctx(struct ieee80211_hw *hw,
        struct ieee80211_vif_chanctx_switch *vifs, int n_vifs,
        enum ieee80211_chanctx_switch_mode mode) { return 0; }

/* Ampdu params */
struct ieee80211_ampdu_params {
    enum { IEEE80211_AMPDU_RX_START, IEEE80211_AMPDU_RX_STOP,
           IEEE80211_AMPDU_TX_START, IEEE80211_AMPDU_TX_START_IMMEDIATE,
           IEEE80211_AMPDU_TX_STOP_CONT,
           IEEE80211_AMPDU_TX_STOP_FLUSH, IEEE80211_AMPDU_TX_STOP_FLUSH_CONT,
           IEEE80211_AMPDU_TX_OPERATIONAL } action;
    struct ieee80211_sta *sta;
    u16 tid;
    u16 *ssn;
    u8  buf_size;
    bool amsdu;
    u16 timeout;
};

/* ------------------------------------------------------------------ */
/*  ieee80211_hw alloc / free                                           */
/* ------------------------------------------------------------------ */

/* Implemented in rtw88_compat.c */
void rtw88_register_hw(struct ieee80211_hw *hw);
void rtw88_unregister_hw(struct ieee80211_hw *hw);
/* rtw88_get_hw: external-linkage accessor for the static g_rtw88_hw pointer.
 * Use this instead of 'extern struct ieee80211_hw *g_rtw88_hw' — the variable
 * has internal linkage so a direct extern declaration is UB and resolves to an
 * arbitrary symbol, producing a garbage pointer and a kernel panic.
 * CRITICAL: declared here (not implicitly) so the compiler knows the return
 * type is a 64-bit pointer, not int. */
struct ieee80211_hw *rtw88_get_hw(void);
/* CRITICAL: must be declared here so the compiler knows the return type is a
 * pointer (64-bit), NOT int. Without this declaration, the compiler emits
 * cltq after the call, truncating the 64-bit return value to 32 bits. */
struct ieee80211_hw *wiphy_to_ieee80211_hw(struct wiphy *wiphy);

static inline struct ieee80211_hw *ieee80211_alloc_hw(size_t priv_data_len,
                                                       const struct ieee80211_ops *ops)
{
    /* Static fallback channel: 2.4 GHz band, CH1 (2412 MHz).
     * rtw_rx_fill_rx_status dereferences hw->conf.chandef.chan unconditionally,
     * so it must never be NULL — even before the driver calls ieee80211_config(). */
    static struct ieee80211_channel s_default_chan = {
        .band        = NL80211_BAND_2GHZ,
        .center_freq = 2412,
        .hw_value    = 1,
        .flags       = 0,
        .max_power   = 20,
    };

    struct ieee80211_hw *hw = (struct ieee80211_hw *)
        kzalloc(sizeof(*hw) + priv_data_len, GFP_KERNEL);
    if (!hw) return NULL;
    hw->priv = (u8 *)hw + sizeof(*hw);
    hw->wiphy = (struct wiphy *)kzalloc(sizeof(struct wiphy), GFP_KERNEL);
    if (!hw->wiphy) { kfree(hw); return NULL; }
    /* Device context for compatibility helpers. The hw accessor returns
     * the registered ieee80211_hw allocation, not a cast of this wiphy. */
    hw->wiphy->_dev = hw->priv;
    hw->ops = ops;
    hw->conf.chandef.chan   = &s_default_chan;
    hw->conf.chandef.width  = NL80211_CHAN_WIDTH_20_NOHT;
    rtw88_register_hw(hw);     /* belt: global fallback */
    return hw;
}

static inline void ieee80211_free_hw(struct ieee80211_hw *hw)
{
    rtw88_unregister_hw(hw);
    if (hw) kfree(hw->wiphy);
    kfree(hw);
}

static inline int ieee80211_register_hw(struct ieee80211_hw *hw)
{
    /* Registration is handled by the kext when it calls rtw_init_hw() */
    return 0;
}

static inline void ieee80211_unregister_hw(struct ieee80211_hw *hw) {}

/* ------------------------------------------------------------------ */
/*  mac80211 callbacks into upper layer (we implement these)           */
/* ------------------------------------------------------------------ */

/* Called when a received frame should be passed up */
void ieee80211_rx_irqsafe(struct ieee80211_hw *hw, struct sk_buff *skb);
void ieee80211_rx_napi(struct ieee80211_hw *hw, struct ieee80211_sta *sta,
                       struct sk_buff *skb, struct napi_struct *napi);

/* TX status reporting */
void ieee80211_tx_status_irqsafe(struct ieee80211_hw *hw, struct sk_buff *skb);
void ieee80211_tx_status(struct ieee80211_hw *hw, struct sk_buff *skb);

/* Free TX skb (no status) */
void ieee80211_free_txskb(struct ieee80211_hw *hw, struct sk_buff *skb);

/* Scan complete notification */
struct cfg80211_scan_info { bool aborted; };
void ieee80211_scan_completed(struct ieee80211_hw *hw,
                               struct cfg80211_scan_info *info);

/* Queue/wake */
void ieee80211_stop_queues(struct ieee80211_hw *hw);
void ieee80211_wake_queues(struct ieee80211_hw *hw);
void ieee80211_stop_queue(struct ieee80211_hw *hw, int queue);
void ieee80211_wake_queue(struct ieee80211_hw *hw, int queue);
void ieee80211_queue_stopped(struct ieee80211_hw *hw, int queue);

/* Schedule TX work */
void ieee80211_schedule_txq(struct ieee80211_hw *hw, struct ieee80211_txq *txq);

/* Connection events */
void ieee80211_connection_loss(struct ieee80211_vif *vif);
void ieee80211_beacon_loss(struct ieee80211_vif *vif);
void ieee80211_cqm_rssi_notify(struct ieee80211_vif *vif,
                                int nl80211_cqm_rssi_threshold_event,
                                s32 rssi_level, gfp_t gfp);

/* Probe response */
struct sk_buff *ieee80211_proberesp_get(struct ieee80211_hw *hw,
                                         struct ieee80211_vif *vif);

/* Channel switch */
void ieee80211_chswitch_done(struct ieee80211_vif *vif, bool success,
                              unsigned int link_id);

/* Iterate active interfaces */
void ieee80211_iterate_active_interfaces_atomic(
    struct ieee80211_hw *hw, unsigned int iter_flags,
    void (*iterator)(void *data, u8 *mac, struct ieee80211_vif *vif),
    void *data);

void ieee80211_iterate_active_interfaces(
    struct ieee80211_hw *hw, unsigned int iter_flags,
    void (*iterator)(void *data, u8 *mac, struct ieee80211_vif *vif),
    void *data);

void ieee80211_iterate_stations_atomic(
    struct ieee80211_hw *hw,
    void (*iterator)(void *data, struct ieee80211_sta *sta),
    void *data);

void ieee80211_iter_keys(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
    void (*iter)(struct ieee80211_hw *hw,
                 struct ieee80211_vif *vif,
                 struct ieee80211_sta *sta,
                 struct ieee80211_key_conf *key,
                 void *data),
    void *iter_data);

void ieee80211_iter_keys_rcu(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
    void (*iter)(struct ieee80211_hw *hw,
                 struct ieee80211_vif *vif,
                 struct ieee80211_sta *sta,
                 struct ieee80211_key_conf *key,
                 void *data),
    void *iter_data);

#define IEEE80211_IFACE_ITER_NORMAL   0
#define IEEE80211_IFACE_ITER_RESUME_ALL 1
#define IEEE80211_IFACE_SKIP_SDATA_NOT_IN_DRIVER 2

/* TXQ drain */
struct sk_buff *ieee80211_tx_dequeue(struct ieee80211_hw *hw,
                                      struct ieee80211_txq *txq);
struct sk_buff *ieee80211_tx_dequeue_ni(struct ieee80211_hw *hw,
                                         struct ieee80211_txq *txq);
bool ieee80211_txq_may_transmit(struct ieee80211_hw *hw,
                                 struct ieee80211_txq *txq);
void ieee80211_txq_schedule_start(struct ieee80211_hw *hw, u8 ac);
void ieee80211_txq_schedule_end(struct ieee80211_hw *hw, u8 ac);
bool ieee80211_txq_is_last(struct ieee80211_hw *hw,
                             struct ieee80211_txq *txq);

/* Misc */
struct sk_buff *ieee80211_beacon_get_tim(struct ieee80211_hw *hw,
                                          struct ieee80211_vif *vif,
                                          u16 *tim_offset, u16 *tim_length,
                                          u32 link_id);
struct sk_buff *ieee80211_nullfunc_get(struct ieee80211_hw *hw,
                                        struct ieee80211_vif *vif,
                                        int link_id, bool qos_ok);
struct sk_buff *ieee80211_pspoll_get(struct ieee80211_hw *hw,
                                      struct ieee80211_vif *vif);
struct sk_buff *ieee80211_probereq_get(struct ieee80211_hw *hw, const u8 *src_addr,
                                        const u8 *ssid, size_t ssid_len,
                                        size_t tailroom);

int ieee80211_sta_ps_transition(struct ieee80211_sta *sta, bool start);
void ieee80211_sta_pspoll(struct ieee80211_sta *sta);
void ieee80211_sta_uapsd_trigger(struct ieee80211_sta *sta, u8 tid);

u8 ieee80211_mcs_to_chains(const void *mcs);
int ieee80211_freq_to_channel(int freq);
int ieee80211_channel_to_frequency(int chan, enum nl80211_band band);

/* WoWLAN */
struct cfg80211_wowlan;

/* SAR (Specific Absorption Rate) */
#define NL80211_SAR_TYPE_POWER  0

struct cfg80211_sar_freq_ranges {
    u32 start_freq;
    u32 end_freq;
};

struct cfg80211_sar_sub_specs {
    u32 freq_range_index;
    s32 power;
};

struct cfg80211_sar_specs {
    u32 type;
    u32 num_sub_specs;
    struct cfg80211_sar_sub_specs sub_specs[0];
};

struct cfg80211_sar_capa {
    u32 type;
    u32 num_freq_ranges;
    const struct cfg80211_sar_freq_ranges *freq_ranges;
};

/* cfg80211_bitrate_mask — per-band legacy/HT/VHT rate mask */
struct cfg80211_bitrate_mask {
    struct {
        u32 legacy;
        u8  ht_mcs[8];
        u16 vht_mcs[8];
    } control[NL80211_NUM_BANDS];
};

/* 802.11 SSID */
#define IEEE80211_MAX_SSID_LEN  32

struct cfg80211_ssid {
    u8  ssid[IEEE80211_MAX_SSID_LEN];
    u8  ssid_len;
};

struct cfg80211_match_set {
    struct cfg80211_ssid ssid;
    u8   bssid[ETH_ALEN];
    s32  rssi_thold;
};

/* NL80211 CQM events */
enum nl80211_cqm_rssi_threshold_event {
    NL80211_CQM_RSSI_THRESHOLD_EVENT_LOW  = 0,
    NL80211_CQM_RSSI_THRESHOLD_EVENT_HIGH = 1,
    NL80211_CQM_RSSI_BEACON_LOSS_EVENT    = 2,
};
struct cfg80211_sar_specs;
struct ieee80211_channel_switch;
struct ieee80211_low_level_stats;

/* Rate control */
#define IEEE80211_TX_RC_MCS           (1 << 0)
#define IEEE80211_TX_RC_VHT_MCS       (1 << 1)
#define IEEE80211_TX_RC_40_MHZ_WIDTH  (1 << 2)
#define IEEE80211_TX_RC_80_MHZ_WIDTH  (1 << 3)
#define IEEE80211_TX_RC_160_MHZ_WIDTH (1 << 4)
#define IEEE80211_TX_RC_SHORT_GI      (1 << 5)
#define IEEE80211_TX_RC_USE_RTS_CTS   (1 << 6)
#define IEEE80211_TX_RC_USE_CTS_PROTECT (1 << 7)
#define IEEE80211_TX_RC_USE_SHORT_PREAMBLE (1 << 8)

/* HT/VHT caps bits */
/* HT RX STBC shift */
#define IEEE80211_HT_CAP_RX_STBC_SHIFT  8

/* HT AMPDU factor/density */
#define IEEE80211_HT_MAX_AMPDU_8K       1
#define IEEE80211_HT_MAX_AMPDU_16K      2
#define IEEE80211_HT_MAX_AMPDU_64K      3   /* 2^(13+3) = 64K */

#define IEEE80211_VHT_MAX_AMPDU_8K      0
#define IEEE80211_VHT_MAX_AMPDU_16K     1
#define IEEE80211_VHT_MAX_AMPDU_32K     2
#define IEEE80211_VHT_MAX_AMPDU_64K     3
#define IEEE80211_VHT_MAX_AMPDU_128K    4
#define IEEE80211_VHT_MAX_AMPDU_256K    5
#define IEEE80211_VHT_MAX_AMPDU_512K    6
#define IEEE80211_VHT_MAX_AMPDU_1024K   7
#define IEEE80211_HT_MPDU_DENSITY_NONE  0
#define IEEE80211_HT_MPDU_DENSITY_0_25  1
#define IEEE80211_HT_MPDU_DENSITY_0_5   2
#define IEEE80211_HT_MPDU_DENSITY_1     3
#define IEEE80211_HT_MPDU_DENSITY_2     4
#define IEEE80211_HT_MPDU_DENSITY_4     5
#define IEEE80211_HT_MPDU_DENSITY_8     6
#define IEEE80211_HT_MPDU_DENSITY_16    7

/* HT/VHT data length */
#define IEEE80211_MAX_DATA_LEN          2304

/* SMPS modes */
enum ieee80211_smps_mode {
    IEEE80211_SMPS_AUTOMATIC = 0,
    IEEE80211_SMPS_OFF       = 1,
    IEEE80211_SMPS_STATIC    = 2,
    IEEE80211_SMPS_DYNAMIC   = 3,
    IEEE80211_SMPS_NUM_MODES,
};

static inline void ieee80211_request_smps(struct ieee80211_vif *vif,
                                           unsigned int link_id,
                                           enum ieee80211_smps_mode smps_mode) {}

#define IEEE80211_HT_CAP_LDPC_CODING    0x0001
#define IEEE80211_HT_CAP_SUP_WIDTH_20_40 0x0002
#define IEEE80211_HT_CAP_SM_PS          0x000C
#define IEEE80211_HT_CAP_GRN_FLD        0x0010
#define IEEE80211_HT_CAP_SGI_20         0x0020
#define IEEE80211_HT_CAP_SGI_40         0x0040
#define IEEE80211_HT_CAP_TX_STBC        0x0080
#define IEEE80211_HT_CAP_RX_STBC        0x0300
#define IEEE80211_HT_CAP_DELAY_BA       0x0400
#define IEEE80211_HT_CAP_MAX_AMSDU      0x0800
#define IEEE80211_HT_CAP_DSSSCCK40      0x1000
#define IEEE80211_HT_MCS_TX_DEFINED     0x01
#define IEEE80211_HT_MCS_TX_RX_DIFF     0x02
#define IEEE80211_HT_MCS_TX_MAX_STREAMS_MASK  0x0C
#define IEEE80211_HT_MCS_TX_MAX_STREAMS_SHIFT 2

/* VHT RXSTBC */
#define IEEE80211_VHT_CAP_RXSTBC_MASK            0x00000700
#define IEEE80211_VHT_CAP_RXSTBC_1               0x00000100

/* VHT MCS map values (2 bits per NSS) */
#define IEEE80211_VHT_MCS_SUPPORT_0_7   0
#define IEEE80211_VHT_MCS_SUPPORT_0_8   1
#define IEEE80211_VHT_MCS_SUPPORT_0_9   2
#define IEEE80211_VHT_MCS_NOT_SUPPORTED 3

#define IEEE80211_VHT_CAP_MAX_MPDU_LENGTH_3895   0
#define IEEE80211_VHT_CAP_MAX_MPDU_LENGTH_7991   1
#define IEEE80211_VHT_CAP_MAX_MPDU_LENGTH_11454  2
#define IEEE80211_VHT_CAP_RXLDPC                 0x00000010
#define IEEE80211_VHT_CAP_SHORT_GI_80            0x00000020
#define IEEE80211_VHT_CAP_SHORT_GI_160           0x00000040
#define IEEE80211_VHT_CAP_TXSTBC                 0x00000080
#define IEEE80211_VHT_CAP_MU_BEAMFORMER_CAPABLE  0x00080000
#define IEEE80211_VHT_CAP_MU_BEAMFORMEE_CAPABLE  0x00100000
#define IEEE80211_VHT_CAP_HTC_VHT                0x00400000
#define IEEE80211_VHT_CAP_MAX_A_MPDU_LENGTH_EXPONENT_MASK 0x03800000
#define IEEE80211_VHT_CAP_MAX_A_MPDU_LENGTH_EXPONENT_SHIFT 23
#define IEEE80211_VHT_CAP_RX_ANTENNA_PATTERN     0x10000000
#define IEEE80211_VHT_CAP_TX_ANTENNA_PATTERN     0x20000000
#define IEEE80211_VHT_CAP_SU_BEAMFORMER_CAPABLE  0x00000800
#define IEEE80211_VHT_CAP_SU_BEAMFORMEE_CAPABLE  0x00001000
#define IEEE80211_VHT_CAP_BEAMFORMEE_STS_SHIFT   13
#define IEEE80211_VHT_CAP_BEAMFORMEE_STS_MASK    (7 << 13)
#define IEEE80211_VHT_CAP_SOUNDING_DIMENSIONS_SHIFT 16
#define IEEE80211_VHT_CAP_SOUNDING_DIMENSIONS_MASK  (7 << 16)

/* Conf TX params */
struct ieee80211_tx_queue_params {
    u16 txop;
    u16 cw_min;
    u16 cw_max;
    u8  aifs;
    bool uapsd;
};

/* Interface limits and combinations (used in mac80211.c / main.c) */
struct ieee80211_iface_limit {
    u16 max;
    u16 types;  /* bitmask of nl80211_iftype bits */
};

struct ieee80211_iface_combination {
    const struct ieee80211_iface_limit *limits;
    u32  n_limits;
    u32  max_interfaces;
    u8   num_different_channels;
    bool beacon_int_infra_match;
    u8   radar_detect_widths;
    u16  radar_detect_regions;
};

/* rate_info — used by rtw_ra_report in main.h */
#define RATE_INFO_FLAGS_MCS       (1 << 0)
#define RATE_INFO_FLAGS_VHT_MCS   (1 << 1)
#define RATE_INFO_FLAGS_SHORT_GI  (1 << 2)
#define RATE_INFO_FLAGS_HE_MCS    (1 << 3)

struct rate_info {
    u32 flags;
    u32 legacy;
    u8  mcs;
    u8  nss;
    u8  bw;
    u8  he_gi;
    u8  he_dcm;
    u8  he_ru_alloc;
    u8  n_bonded_ch;
};

static inline u32 cfg80211_calculate_bitrate(struct rate_info *rate)
{
    if (rate->flags & RATE_INFO_FLAGS_VHT_MCS)
        return rate->nss * (rate->mcs + 1) * 1000; /* rough estimate */
    if (rate->flags & RATE_INFO_FLAGS_MCS)
        return (rate->mcs + 1) * 1000;
    return rate->legacy * 100; /* legacy is in 100kbps units */
}

struct station_info {
    u64              filled;
    struct rate_info txrate;
    struct rate_info rxrate;
    u32  inactive_time;
    u64  rx_bytes;
    u64  tx_bytes;
    u32  rx_packets;
    u32  tx_packets;
    s8   signal;
    s8   signal_avg;
};

struct ieee80211_prep_tx_info {
    u16 duration;
    bool success;
};

/* ------------------------------------------------------------------ */
/*  Regulatory                                                          */
/* ------------------------------------------------------------------ */

#define IEEE80211_CHAN_NO_80MHZ   (1 << 11)
#define IEEE80211_CHAN_NO_160MHZ  (1 << 12)

#define REGULATORY_STRICT_REG           (1 << 0)
#define REGULATORY_COUNTRY_IE_IGNORE    (1 << 2)

/* nl80211_dfs_regions — used by rtw_regulatory in main.h */
enum nl80211_dfs_regions {
    NL80211_DFS_UNSET = 0,
    NL80211_DFS_FCC   = 1,
    NL80211_DFS_ETSI  = 2,
    NL80211_DFS_JP    = 3,
};

enum nl80211_reg_initiator {
    NL80211_REGDOM_SET_BY_CORE       = 0,
    NL80211_REGDOM_SET_BY_USER       = 1,
    NL80211_REGDOM_SET_BY_DRIVER     = 2,
    NL80211_REGDOM_SET_BY_COUNTRY_IE = 3,
};

struct regulatory_request {
    enum nl80211_reg_initiator initiator;
    char  alpha2[2];
    enum nl80211_dfs_regions   dfs_region;
};

/* cfg80211_sched_scan_plan — used by rtw_hw_scan in main.h */
struct cfg80211_sched_scan_plan {
    u32 interval;
    u32 iterations;
};

/* wait_queue_head_t — used in main.h for FW/calibration completion */
typedef struct {
    IOLock *lock;
} wait_queue_head_t;

static inline void init_waitqueue_head(wait_queue_head_t *wq)
{
    wq->lock = IOLockAlloc();
}
#define wake_up(wq)            do {} while (0)
#define wake_up_all(wq)        do {} while (0)
#define wake_up_interruptible(wq) do {} while (0)
#define wait_event_interruptible(wq, cond)  ({ (void)(cond); 0; })
#define wait_event_timeout(wq, cond, to)    ({ (void)(cond); 1; })


/* Out-of-line compatibility helpers: declarations must be visible to C callers. */
void ieee80211_queue_work(struct ieee80211_hw *, struct work_struct *);
void ieee80211_queue_delayed_work(struct ieee80211_hw *, struct delayed_work *, unsigned long);
struct ieee80211_sta *ieee80211_find_sta(struct ieee80211_vif *, const u8 *);
struct ieee80211_sta *ieee80211_find_sta_by_ifaddr(struct ieee80211_hw *, const u8 *, const u8 *);
void ieee80211_purge_tx_queue(struct ieee80211_hw *, struct sk_buff_head *);
void ieee80211_restart_hw(struct ieee80211_hw *);
int ieee80211_start_tx_ba_session(struct ieee80211_sta *, u16, u16);
void ieee80211_stop_tx_ba_cb_irqsafe(struct ieee80211_vif *, const u8 *, u16);
void ieee80211_tx_info_clear_status(struct ieee80211_tx_info *);
void ieee80211_txq_get_depth(struct ieee80211_txq *, unsigned long *, unsigned long *);
u8 ieee80211_vif_type_p2p(struct ieee80211_vif *);
int cfg80211_get_ies_channel_number(const u8 *, size_t, enum nl80211_band);
bool cfg80211_ssid_eq(struct cfg80211_ssid *, struct cfg80211_ssid *);
int regulatory_hint(struct wiphy *, const char *);

#endif /* _RTW88_COMPAT_MAC80211_H */
