// SPDX-License-Identifier: GPL-2.0
/*
 * Station-mode MLME: joining and leaving one network on one non-MLO station
 * interface. This is the part of net/mac80211/mlme.c the driver cannot do
 * without, and the order of driver calls follows it (ieee80211_prep_connection,
 * ieee80211_auth, ieee80211_send_assoc, ieee80211_assoc_success,
 * ieee80211_set_associated, ieee80211_set_disassoc).
 *
 * Scope so far: open-system authentication and association, 802.11n on a
 * 20 MHz channel, and WPA2-PSK with CCMP: the supplicant side of the 4-way
 * and group key handshakes (IEEE 802.11 12.7.6/12.7.7) and installing the
 * keys in the driver. The data path is not here yet.
 *
 * Everything runs under the wiphy mutex, as in mac80211: commands take it,
 * received frames are queued and handled from a wiphy work.
 */
#include "rtw89_net80211.h"
#include "rtw89_crypto.h"

#define MLME_AUTH_TIMEOUT   (HZ / 2)
#define MLME_ASSOC_TIMEOUT  (HZ / 2)
#define MLME_MAX_TRIES      3
#define MLME_KEY_TIMEOUT    (10 * HZ)   /* for the whole key handshake */
#define MLME_MAX_IES        1024

static struct {
    bool running;
    struct rtw89_m80211_local *local;
    struct ieee80211_hw *hw;
    struct ieee80211_vif *vif;

    enum rtw89_mlme_state state;
    int last_error;
    u32 eapol_rx;
    unsigned int tries;

    /* the network being joined */
    struct rtw89_mlme_bss bss;
    u8 ies[MLME_MAX_IES];
    struct ieee80211_channel *chan;
    struct ieee80211_supported_band *sband;
    bool ht;                    /* AP and card both do 802.11n */
    bool vht;                   /* ... and 802.11ac */
    struct cfg80211_chan_def chandef;   /* the channel and width we operate on */
    bool wmm;
    bool rsn;
    u8 group_cipher[4];         /* group data cipher suite from the AP's RSN IE */

    struct ieee80211_chanctx_conf *chanctx;
    bool chanctx_assigned;
    struct ieee80211_sta *sta;
    enum ieee80211_sta_state sta_state;
    u16 aid;

    /* WPA2-PSK */
    bool have_pmk;
    u8 pmk[32];
    struct mlme_ptk {
        u8 kck[16];             /* key confirmation key: EAPOL MICs */
        u8 kek[16];             /* key encryption key: unwraps the group key */
        u8 tk[16];              /* temporal key: CCMP */
    } ptk, tptk;                /* tptk: derived from a message 1, becomes ptk
                                 * once the AP proves it has the same key */
    bool ptk_valid, tptk_valid;
    u8 anonce[32];
    u8 snonce[32];
    bool snonce_valid;          /* kept while the AP retries message 1 */
    u8 replay[8];               /* last replay counter accepted from the AP */
    bool replay_valid;
    struct ieee80211_key_conf *ptk_conf;
    struct ieee80211_key_conf *gtk_conf[4];     /* by key id; the AP alternates */

    /* BlockAck sessions */
    struct mlme_tx_ba {
        u8 state;               /* MLME_BA_* */
        u8 dialog;
        u8 tries;               /* requests since the AP last agreed */
        u16 ssn;
        unsigned long deadline; /* for the AP's answer */
        unsigned long last_try;
    } tx_ba[8];                 /* no aggregation on TIDs 8-15 */
    unsigned long tx_ba_start_req, tx_ba_stop_req;  /* TID bits, set by driver threads */
    u16 rx_ba;                  /* TIDs the AP may send aggregates on */
    u8 dialog_token;
    struct wiphy_work ba_work;
    struct wiphy_delayed_work ba_timeout_work;

    void (*notify)(void);
    void (*tx_tap)(const u8 *frame, size_t len);

    struct sk_buff_head rxq;
    struct wiphy_work rx_work;
    struct wiphy_work lost_work;
    struct wiphy_delayed_work timeout_work;
} mlme;

/* The RSN element we put in the association request. Message 2 of the 4-way
 * handshake must repeat it byte for byte. */
static const u8 mlme_rsn_ie[] = {
    WLAN_EID_RSN, 20,
    1, 0,                       /* version */
    0x00, 0x0f, 0xac, 4,        /* group cipher: CCMP */
    1, 0, 0x00, 0x0f, 0xac, 4,  /* pairwise: CCMP */
    1, 0, 0x00, 0x0f, 0xac, 2,  /* AKM: PSK */
    0, 0,                       /* capabilities */
};

static void mlme_set_state(enum rtw89_mlme_state state)
{
    mlme.state = state;
    if (mlme.notify)
        mlme.notify();
}

#define mlme_info(fmt, ...) IOLog("[rtw89 mlme] " fmt "\n", ##__VA_ARGS__)

/* ------------------------------------------------------------------ */
/*  Helpers                                                             */
/* ------------------------------------------------------------------ */

/* EDCA contention window from its exponent */
static u16 ecw2cw(u8 ecw)
{
    return (u16)((1u << ecw) - 1);
}

static const struct element *mlme_find_elem(u8 eid, const u8 *ies, size_t len)
{
    return ies ? cfg80211_find_elem(eid, ies, len) : NULL;
}

/* An element from the association response, else from the beacon/probe response. */
static const struct element *mlme_elem(u8 eid, const u8 *resp, size_t resp_len)
{
    const struct element *elem = mlme_find_elem(eid, resp, resp_len);

    return elem ? elem : mlme_find_elem(eid, mlme.bss.ies, mlme.bss.ies_len);
}

static const struct element *mlme_wmm_param(const u8 *ies, size_t len)
{
    const struct element *elem;

    /* WMM information (subtype 0) and parameter (subtype 1) share OUI and type. */
    for_each_element_id(elem, WLAN_EID_VENDOR_SPECIFIC, ies, len) {
        if (elem->datalen >= sizeof(struct ieee80211_wmm_param_ie) - 2 &&
            elem->data[0] == 0x00 && elem->data[1] == 0x50 && elem->data[2] == 0xf2 &&
            elem->data[3] == WLAN_OUI_TYPE_MICROSOFT_WMM && elem->data[4] == 1)
            return elem;
    }
    return NULL;
}

/*
 * Supported and basic rates as bitmaps over @sband->bitrates, from the
 * Supported Rates and Extended Supported Rates elements (ieee80211_get_rates).
 */
static void mlme_parse_rates(const u8 *ies, size_t len, u32 *rates, u32 *basic)
{
    static const u8 eids[] = { WLAN_EID_SUPP_RATES, WLAN_EID_EXT_SUPP_RATES };
    const struct element *elem;
    int e, i, j;

    *rates = 0;
    *basic = 0;
    for (e = 0; e < ARRAY_SIZE(eids); e++) {
        elem = mlme_find_elem(eids[e], ies, len);
        if (!elem)
            continue;
        for (i = 0; i < elem->datalen; i++) {
            /* 500 kbit/s units on the air, 100 kbit/s in the band table */
            int rate = (elem->data[i] & 0x7f) * 5;

            for (j = 0; j < mlme.sband->n_bitrates; j++) {
                if (mlme.sband->bitrates[j].bitrate != rate)
                    continue;
                *rates |= BIT(j);
                if (elem->data[i] & 0x80)
                    *basic |= BIT(j);
            }
        }
    }
}

/* What the AP's RSN element allows us to use. Returns 0 if WPA2-PSK/CCMP works. */
static int mlme_check_rsn(const struct element *rsn)
{
    static const u8 suite_ccmp[4] = { 0x00, 0x0f, 0xac, 4 };
    static const u8 suite_psk[4] = { 0x00, 0x0f, 0xac, 2 };
    const u8 *p = rsn->data, *end = rsn->data + rsn->datalen;
    bool ccmp = false, psk = false;
    u16 count, caps = 0;

    if (end - p < 8 || get_unaligned_le16(p) != 1)
        return -EINVAL;
    p += 2;
    memcpy(mlme.group_cipher, p, 4);
    p += 4;

    count = get_unaligned_le16(p);
    p += 2;
    if (end - p < count * 4)
        return -EINVAL;
    for (; count; count--, p += 4)
        ccmp |= !memcmp(p, suite_ccmp, 4);

    if (end - p < 2)
        return -EINVAL;
    count = get_unaligned_le16(p);
    p += 2;
    if (end - p < count * 4)
        return -EINVAL;
    for (; count; count--, p += 4)
        psk |= !memcmp(p, suite_psk, 4);

    if (end - p >= 2)
        caps = get_unaligned_le16(p);

    if (!ccmp) {
        mlme_info("the network does not offer CCMP as pairwise cipher");
        return -EOPNOTSUPP;
    }
    if (memcmp(mlme.group_cipher, suite_ccmp, 4)) {
        mlme_info("the network's group cipher is not CCMP (%02x-%02x-%02x:%u)",
                  mlme.group_cipher[0], mlme.group_cipher[1], mlme.group_cipher[2],
                  mlme.group_cipher[3]);
        return -EOPNOTSUPP;
    }
    if (!psk) {
        mlme_info("the network does not offer WPA2-PSK (WPA3-only or enterprise)");
        return -EOPNOTSUPP;
    }
    if (caps & BIT(6)) {    /* RSN capabilities: management frame protection required */
        mlme_info("the network requires management frame protection");
        return -EOPNOTSUPP;
    }
    return 0;
}

static struct sk_buff *mlme_alloc_frame(size_t len)
{
    struct sk_buff *skb = dev_alloc_skb(mlme.hw->extra_tx_headroom + len);

    if (skb)
        skb_reserve(skb, mlme.hw->extra_tx_headroom);
    return skb;
}

/* Hand a management frame to the driver, as ieee80211_tx_skb() ends up doing. */
static void mlme_tx_mgmt(struct sk_buff *skb)
{
    struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
    struct ieee80211_tx_control control = { .sta = mlme.sta };

    memset(info, 0, sizeof(*info));
    info->control.vif = mlme.vif;
    info->band = mlme.chan->band;
    info->hw_queue = mlme.vif->hw_queue[IEEE80211_AC_VO];
    skb->priority = 7;
    skb_set_queue_mapping(skb, IEEE80211_AC_VO);

    if (mlme.tx_tap)
        mlme.tx_tap(skb->data, skb->len);
    mlme.local->ops->tx(mlme.hw, &control, skb);
}

static struct ieee80211_mgmt *mlme_mgmt_header(struct sk_buff *skb, u16 stype, size_t len)
{
    struct ieee80211_mgmt *mgmt = skb_put_zero(skb, len);

    mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT | stype);
    memcpy(mgmt->da, mlme.bss.bssid, ETH_ALEN);
    memcpy(mgmt->sa, mlme.vif->addr, ETH_ALEN);
    memcpy(mgmt->bssid, mlme.bss.bssid, ETH_ALEN);
    return mgmt;
}

static int mlme_sta_move(enum ieee80211_sta_state new_state)
{
    int ret;

    ret = mlme.local->ops->sta_state(mlme.hw, mlme.vif, mlme.sta, mlme.sta_state, new_state);
    if (ret) {
        mlme_info("driver refused station state %d -> %d: %d", mlme.sta_state, new_state, ret);
        return ret;
    }
    mlme.sta_state = new_state;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Keys                                                                */
/* ------------------------------------------------------------------ */

/* Give a CCMP key to the driver. @sta is NULL for the group key. */
static int mlme_key_install(struct ieee80211_key_conf **slot, struct ieee80211_sta *sta,
                            const u8 *key, s8 keyidx)
{
    struct ieee80211_key_conf *conf;
    int ret;

    conf = kzalloc(sizeof(*conf) + WLAN_KEY_LEN_CCMP, GFP_KERNEL);
    if (!conf)
        return -ENOMEM;
    conf->cipher = WLAN_CIPHER_SUITE_CCMP;
    conf->keyidx = keyidx;
    conf->keylen = WLAN_KEY_LEN_CCMP;
    conf->icv_len = IEEE80211_CCMP_MIC_LEN;
    conf->iv_len = IEEE80211_CCMP_HDR_LEN;
    conf->link_id = -1;
    if (sta)
        conf->flags = IEEE80211_KEY_FLAG_PAIRWISE;
    memcpy(conf->key, key, WLAN_KEY_LEN_CCMP);

    ret = mlme.local->ops->set_key(mlme.hw, SET_KEY, mlme.vif, sta, conf);
    if (!ret)
        ret = rtw89_m80211_key_add(mlme.hw, mlme.vif, sta, conf);
    if (ret) {
        memset(conf, 0, sizeof(*conf) + WLAN_KEY_LEN_CCMP);
        kfree(conf);
        return ret;
    }
    *slot = conf;
    return 0;
}

/* True if @conf is already this very key. */
static bool mlme_key_is(const struct ieee80211_key_conf *conf, const u8 *key, s8 keyidx)
{
    return conf && conf->keyidx == keyidx &&
           rtw89_crypto_equal(conf->key, key, WLAN_KEY_LEN_CCMP);
}

static void mlme_key_remove(struct ieee80211_key_conf **slot, struct ieee80211_sta *sta)
{
    struct ieee80211_key_conf *conf = *slot;

    if (!conf)
        return;
    *slot = NULL;
    mlme.local->ops->set_key(mlme.hw, DISABLE_KEY, mlme.vif, sta, conf);
    rtw89_m80211_key_del(mlme.hw, conf);
    /* the key itself goes now; a frame still queued may point at the rest */
    memset(conf->key, 0, WLAN_KEY_LEN_CCMP);
    rtw89_m80211_free_later(mlme.hw, conf, NULL);
}

/* Install the group key with id @idx unless it is the one already there. */
static int mlme_gtk_install(int idx, const u8 *gtk, u64 rsc)
{
    int ret;

    if (mlme_key_is(mlme.gtk_conf[idx], gtk, (s8)idx))
        return 0;
    if (mlme.gtk_conf[idx]) {
        rtw89_data_set_rx_key(idx, false, 0);
        mlme_key_remove(&mlme.gtk_conf[idx], NULL);
    }
    ret = mlme_key_install(&mlme.gtk_conf[idx], NULL, gtk, (s8)idx);
    if (!ret)
        rtw89_data_set_rx_key(idx, true, rsc);
    return ret;
}

/* ------------------------------------------------------------------ */
/*  Leaving (ieee80211_set_disassoc / ieee80211_destroy_auth_data)      */
/* ------------------------------------------------------------------ */

static void mlme_send_deauth(u16 reason)
{
    const size_t len = offsetof(struct ieee80211_mgmt, u.deauth) + sizeof(__le16);
    struct sk_buff *skb = mlme_alloc_frame(len);
    struct ieee80211_mgmt *mgmt;

    if (!skb)
        return;
    mgmt = mlme_mgmt_header(skb, IEEE80211_STYPE_DEAUTH, len);
    mgmt->u.deauth.reason_code = cpu_to_le16(reason);
    mlme_tx_mgmt(skb);

    /* make sure it is on the air before the station goes away */
    if (mlme.local->ops->flush)
        mlme.local->ops->flush(mlme.hw, mlme.vif, BIT(mlme.hw->queues) - 1, false);
}

static void mlme_ba_teardown(void);

/* Undo whatever the join got to. @deauth_reason: tell the AP first if non-zero. */
static void mlme_teardown(u16 deauth_reason)
{
    const struct ieee80211_ops *ops = mlme.local->ops;
    struct ieee80211_bss_conf *conf = &mlme.vif->bss_conf;
    bool was_assoc = mlme.vif->cfg.assoc;
    u64 changed = BSS_CHANGED_BSSID;

    if (mlme.state == RTW89_MLME_IDLE)
        return;

    int i;

    wiphy_delayed_work_cancel(mlme.hw->wiphy, &mlme.timeout_work);

    /* no more data in either direction, and nothing left queued for the AP */
    rtw89_data_detach();
    mlme_ba_teardown();

    if (deauth_reason && mlme.sta && mlme.sta_state >= IEEE80211_STA_NONE)
        mlme_send_deauth(deauth_reason);

    /* clear the AP address only after the frames that need it are built */
    eth_zero_addr(to_mvif(mlme.vif)->bssid);
    eth_zero_addr(mlme.vif->cfg.ap_addr);
    mlme.vif->cfg.ssid_len = 0;

    /* keys go before the station they belong to */
    mlme_key_remove(&mlme.ptk_conf, mlme.sta);
    for (i = 0; i < ARRAY_SIZE(mlme.gtk_conf); i++)
        mlme_key_remove(&mlme.gtk_conf[i], NULL);
    memset(&mlme.ptk, 0, sizeof(mlme.ptk));
    memset(&mlme.tptk, 0, sizeof(mlme.tptk));
    mlme.ptk_valid = false;
    mlme.tptk_valid = false;
    mlme.snonce_valid = false;
    mlme.replay_valid = false;

    if (mlme.sta) {
        /* sta_info_flush(): one state at a time, down to "does not exist" */
        while (mlme.sta_state > IEEE80211_STA_NOTEXIST) {
            enum ieee80211_sta_state next = mlme.sta_state - 1;

            /* the iterators stop seeing it once the driver is told it is gone */
            if (next == IEEE80211_STA_NOTEXIST)
                rtw89_m80211_sta_set_uploaded(mlme.sta, false);
            if (ops->sta_state(mlme.hw, mlme.vif, mlme.sta, mlme.sta_state, next))
                mlme_info("driver failed station state %d -> %d", mlme.sta_state, next);
            mlme.sta_state = next;
        }
        rtw89_m80211_sta_free(mlme.sta);
        mlme.sta = NULL;
    }

    if (was_assoc) {
        mlme.vif->cfg.assoc = false;
        mlme.vif->cfg.aid = 0;
        conf->qos = false;
        conf->ht_operation_mode = 0;
        conf->use_cts_prot = false;
        conf->use_short_preamble = false;
        conf->use_short_slot = false;
        changed |= BSS_CHANGED_QOS | BSS_CHANGED_HT | BSS_CHANGED_ERP_CTS_PROT |
                   BSS_CHANGED_ERP_PREAMBLE | BSS_CHANGED_ERP_SLOT;
        if (ops->vif_cfg_changed)
            ops->vif_cfg_changed(mlme.hw, mlme.vif, BSS_CHANGED_ASSOC);
    }
    if (ops->link_info_changed)
        ops->link_info_changed(mlme.hw, mlme.vif, conf, changed);

    /* ieee80211_link_release_channel() */
    if (mlme.chanctx) {
        if (mlme.chanctx_assigned) {
            ops->unassign_vif_chanctx(mlme.hw, mlme.vif, conf, mlme.chanctx);
            conf->chanctx_conf = NULL;
            mlme.chanctx_assigned = false;
        }
        ops->remove_chanctx(mlme.hw, mlme.chanctx);
        kfree(mlme.chanctx);
        mlme.chanctx = NULL;
    }
    memset(&conf->chanreq, 0, sizeof(conf->chanreq));

    mlme_set_state(RTW89_MLME_IDLE);
}

static void mlme_fail(int error, const char *what)
{
    mlme_info("%s (%d)", what, error);
    mlme.last_error = error;
    mlme_teardown(0);
}

/* ------------------------------------------------------------------ */
/*  Authentication                                                      */
/* ------------------------------------------------------------------ */

static void mlme_send_auth(void)
{
    const size_t len = offsetof(struct ieee80211_mgmt, u.auth.variable);
    struct sk_buff *skb = mlme_alloc_frame(len);
    struct ieee80211_mgmt *mgmt;

    if (!skb)
        return;
    mgmt = mlme_mgmt_header(skb, IEEE80211_STYPE_AUTH, len);
    mgmt->u.auth.auth_alg = cpu_to_le16(WLAN_AUTH_OPEN);
    mgmt->u.auth.auth_transaction = cpu_to_le16(1);
    mgmt->u.auth.status_code = cpu_to_le16(0);
    mlme_tx_mgmt(skb);

    mlme.tries++;
    wiphy_delayed_work_queue(mlme.hw->wiphy, &mlme.timeout_work, MLME_AUTH_TIMEOUT);
}

/* ------------------------------------------------------------------ */
/*  Association                                                         */
/* ------------------------------------------------------------------ */

/* Our HT capabilities as advertised for a 20 MHz association (ieee80211_add_ht_ie). */
/* ------------------------------------------------------------------ */
/*  Channel width (ieee80211_determine_ap_chan() and what it calls)     */
/* ------------------------------------------------------------------ */

static unsigned int mlme_chandef_mhz(const struct cfg80211_chan_def *def)
{
    switch (def->width) {
    case NL80211_CHAN_WIDTH_40:
        return 40;
    case NL80211_CHAN_WIDTH_80:
        return 80;
    case NL80211_CHAN_WIDTH_160:
        return 160;
    default:
        return 20;
    }
}

/* The control channel is one of the 20 MHz channels under @def. */
static bool mlme_chandef_valid(const struct cfg80211_chan_def *def)
{
    unsigned int width = mlme_chandef_mhz(def);
    unsigned int first = def->center_freq1 - width / 2 + 10;   /* lowest 20 MHz channel */
    unsigned int last = def->center_freq1 + width / 2 - 10;

    return def->width != NL80211_CHAN_WIDTH_80P80 &&
           def->chan->center_freq >= first && def->chan->center_freq <= last &&
           !((def->chan->center_freq - first) % 20);
}

/* ... and every one of them exists and may be used. */
static bool mlme_chandef_usable(const struct cfg80211_chan_def *def)
{
    unsigned int width = mlme_chandef_mhz(def), freq;
    unsigned int first = def->center_freq1 - width / 2 + 10;
    unsigned int last = def->center_freq1 + width / 2 - 10;
    struct ieee80211_channel *chan;

    if (!mlme_chandef_valid(def))
        return false;

    for (freq = first; freq <= last; freq += 20) {
        chan = ieee80211_get_channel(mlme.hw->wiphy, (int)freq);
        if (!chan || (chan->flags & IEEE80211_CHAN_DISABLED))
            return false;
    }
    return true;
}

/* One step narrower, around the control channel (ieee80211_chandef_downgrade()). */
static void mlme_chandef_downgrade(struct cfg80211_chan_def *def)
{
    int tmp;

    switch (def->width) {
    case NL80211_CHAN_WIDTH_160:
        /* which 80 MHz half the control channel is in */
        tmp = (70 + (int)def->chan->center_freq - (int)def->center_freq1) / 20 / 4;
        def->center_freq1 = def->center_freq1 - 40 + 80 * tmp;
        def->width = NL80211_CHAN_WIDTH_80;
        break;
    case NL80211_CHAN_WIDTH_80:
        tmp = (30 + (int)def->chan->center_freq - (int)def->center_freq1) / 20 / 2;
        def->center_freq1 = def->center_freq1 - 20 + 40 * tmp;
        def->width = NL80211_CHAN_WIDTH_40;
        break;
    default:
        def->center_freq1 = def->chan->center_freq;
        def->width = NL80211_CHAN_WIDTH_20;
        break;
    }
    def->center_freq2 = 0;
}

/*
 * The channel the AP operates on, from its HT and VHT operation elements, cut
 * down to what this card and the regulatory domain allow.
 */
static void mlme_determine_chandef(struct cfg80211_chan_def *def)
{
    const struct ieee80211_ht_operation *ht_oper = NULL;
    const struct ieee80211_vht_operation *vht_oper = NULL;
    const struct element *elem;
    struct cfg80211_chan_def wide;
    unsigned int max = 20;

    cfg80211_chandef_create(def, mlme.chan, mlme.ht ? NL80211_CHAN_HT20 : NL80211_CHAN_NO_HT);
    if (!mlme.ht)
        return;

    elem = mlme_find_elem(WLAN_EID_HT_OPERATION, mlme.bss.ies, mlme.bss.ies_len);
    if (elem && elem->datalen >= sizeof(*ht_oper))
        ht_oper = (const void *)elem->data;
    if (!ht_oper)
        return;

    /* ieee80211_chandef_ht_oper(): a secondary channel above or below */
    if (mlme.sband->ht_cap.cap & IEEE80211_HT_CAP_SUP_WIDTH_20_40) {
        max = 40;
        switch (ht_oper->ht_param & IEEE80211_HT_PARAM_CHA_SEC_OFFSET) {
        case IEEE80211_HT_PARAM_CHA_SEC_ABOVE:
            cfg80211_chandef_create(def, mlme.chan, NL80211_CHAN_HT40PLUS);
            break;
        case IEEE80211_HT_PARAM_CHA_SEC_BELOW:
            cfg80211_chandef_create(def, mlme.chan, NL80211_CHAN_HT40MINUS);
            break;
        }
    }

    elem = mlme_find_elem(WLAN_EID_VHT_OPERATION, mlme.bss.ies, mlme.bss.ies_len);
    if (mlme.vht && elem && elem->datalen >= sizeof(*vht_oper))
        vht_oper = (const void *)elem->data;
    if (vht_oper) {
        /* ieee80211_chandef_vht_oper(), for a card without 160 MHz: the first
         * centre frequency is that of the 80 MHz channel the control channel
         * is in, whatever wider channel the AP may have on top of it */
        max = 80;
        wide = *def;
        switch (vht_oper->chan_width) {
        case IEEE80211_VHT_CHANWIDTH_USE_HT:
            break;
        case IEEE80211_VHT_CHANWIDTH_80MHZ:
            wide.width = NL80211_CHAN_WIDTH_80;
            wide.center_freq1 = (u32)ieee80211_channel_to_frequency(vht_oper->center_freq_seg0_idx,
                                                                   mlme.chan->band);
            break;
        case IEEE80211_VHT_CHANWIDTH_160MHZ:
            /* deprecated encoding: the centre of all 160 MHz */
            wide.width = NL80211_CHAN_WIDTH_160;
            wide.center_freq1 = (u32)ieee80211_channel_to_frequency(vht_oper->center_freq_seg0_idx,
                                                                   mlme.chan->band);
            break;
        default:
            /* 80+80, in the old encoding: the first segment is ours */
            wide.width = NL80211_CHAN_WIDTH_80;
            wide.center_freq1 = (u32)ieee80211_channel_to_frequency(vht_oper->center_freq_seg0_idx,
                                                                   mlme.chan->band);
            break;
        }
        if (mlme_chandef_mhz(&wide) > mlme_chandef_mhz(def)) {
            struct cfg80211_chan_def narrow = wide;

            /* the 40 MHz channel of the HT element must be part of it */
            while (mlme_chandef_valid(&wide) && mlme_chandef_mhz(&narrow) > mlme_chandef_mhz(def))
                mlme_chandef_downgrade(&narrow);
            if (mlme_chandef_valid(&wide) && narrow.center_freq1 == def->center_freq1) {
                *def = wide;
            } else {
                mlme_info("the AP's 802.11ac channel is invalid or does not agree with its "
                          "802.11n one: not using 802.11ac");
                mlme.vht = false;
                max = 40;
            }
        }
    }

    while (mlme_chandef_mhz(def) > max || !mlme_chandef_usable(def)) {
        if (def->width == NL80211_CHAN_WIDTH_20 || def->width == NL80211_CHAN_WIDTH_20_NOHT)
            break;
        mlme_chandef_downgrade(def);
    }
}

static void mlme_own_ht_cap(struct ieee80211_sta_ht_cap *own)
{
    *own = mlme.sband->ht_cap;
    /* 40 MHz only where we really use it: some APs never fall back otherwise */
    if (mlme_chandef_mhz(&mlme.chandef) < 40)
        own->cap &= ~(IEEE80211_HT_CAP_SUP_WIDTH_20_40 | IEEE80211_HT_CAP_SGI_40 |
                      IEEE80211_HT_CAP_DSSSCCK40);
    /* spatial multiplexing power save disabled: all chains always listening */
    own->cap &= ~IEEE80211_HT_CAP_SM_PS;
    own->cap |= WLAN_HT_CAP_SM_PS_DISABLED << IEEE80211_HT_CAP_SM_PS_SHIFT;
}

static void mlme_send_assoc(void)
{
    static const u8 wmm_info[] = {
        WLAN_EID_VENDOR_SPECIFIC, 7, 0x00, 0x50, 0xf2, 2, 0, 1, 0,
    };
    struct ieee80211_mgmt *mgmt;
    struct sk_buff *skb;
    u32 ap_rates, ap_basic, rates;
    u16 capab = WLAN_CAPABILITY_ESS;
    u8 *pos, *count;
    int i, n;

    skb = mlme_alloc_frame(offsetof(struct ieee80211_mgmt, u.assoc_req.variable) + 256);
    if (!skb)
        return;

    if (mlme.sband->band == NL80211_BAND_2GHZ)
        capab |= WLAN_CAPABILITY_SHORT_SLOT_TIME | WLAN_CAPABILITY_SHORT_PREAMBLE;
    if (mlme.bss.capability & WLAN_CAPABILITY_PRIVACY)
        capab |= WLAN_CAPABILITY_PRIVACY;

    mgmt = mlme_mgmt_header(skb, IEEE80211_STYPE_ASSOC_REQ,
                            offsetof(struct ieee80211_mgmt, u.assoc_req.variable));
    mgmt->u.assoc_req.capab_info = cpu_to_le16(capab);
    mgmt->u.assoc_req.listen_interval = cpu_to_le16(mlme.hw->conf.listen_interval);

    /* SSID */
    pos = skb_put(skb, 2 + mlme.bss.ssid_len);
    *pos++ = WLAN_EID_SSID;
    *pos++ = mlme.bss.ssid_len;
    memcpy(pos, mlme.bss.ssid, mlme.bss.ssid_len);

    /* The rates both sides support: eight in Supported Rates, the rest in
     * Extended Supported Rates (ieee80211_assoc_add_rates). */
    mlme_parse_rates(mlme.bss.ies, mlme.bss.ies_len, &ap_rates, &ap_basic);
    rates = ap_rates ? ap_rates : (u32)BIT(mlme.sband->n_bitrates) - 1;
    pos = skb_put(skb, 2);
    *pos++ = WLAN_EID_SUPP_RATES;
    count = pos;
    for (i = 0, n = 0; i < mlme.sband->n_bitrates; i++) {
        if (!(rates & BIT(i)))
            continue;
        if (n == 8) {
            *count = 8;
            pos = skb_put(skb, 2);
            *pos++ = WLAN_EID_EXT_SUPP_RATES;
            count = pos;
        }
        *(u8 *)skb_put(skb, 1) = (u8)(mlme.sband->bitrates[i].bitrate / 5);
        n++;
    }
    *count = n > 8 ? n - 8 : n;

    /* RSN: WPA2-PSK with CCMP for both pairwise and group */
    if (mlme.rsn)
        skb_put_data(skb, mlme_rsn_ie, sizeof(mlme_rsn_ie));

    if (mlme.ht) {
        struct ieee80211_sta_ht_cap own;
        struct ieee80211_ht_cap *ht;

        mlme_own_ht_cap(&own);
        pos = skb_put_zero(skb, 2 + sizeof(*ht));
        *pos++ = WLAN_EID_HT_CAPABILITY;
        *pos++ = sizeof(*ht);
        ht = (struct ieee80211_ht_cap *)pos;
        ht->cap_info = cpu_to_le16(own.cap);
        ht->ampdu_params_info = own.ampdu_factor |
            (own.ampdu_density << IEEE80211_HT_AMPDU_PARM_DENSITY_SHIFT);
        memcpy(&ht->mcs, &own.mcs, sizeof(ht->mcs));
    }

    if (mlme.vht) {
        /* ieee80211_add_vht_ie(): our capabilities, not better than the AP's */
        const struct element *elem = mlme_find_elem(WLAN_EID_VHT_CAPABILITY, mlme.bss.ies,
                                                    mlme.bss.ies_len);
        u32 ap_cap = le32_to_cpu(((const struct ieee80211_vht_cap *)elem->data)->vht_cap_info);
        u32 cap = mlme.sband->vht_cap.cap;
        struct ieee80211_vht_cap *vht;

        cap &= ~(IEEE80211_VHT_CAP_SHORT_GI_160 | IEEE80211_VHT_CAP_SUPP_CHAN_WIDTH_MASK);
        if (!(ap_cap & IEEE80211_VHT_CAP_SU_BEAMFORMER_CAPABLE))
            cap &= ~(IEEE80211_VHT_CAP_SU_BEAMFORMEE_CAPABLE |
                     IEEE80211_VHT_CAP_MU_BEAMFORMEE_CAPABLE);
        else if (!(ap_cap & IEEE80211_VHT_CAP_MU_BEAMFORMER_CAPABLE))
            cap &= ~IEEE80211_VHT_CAP_MU_BEAMFORMEE_CAPABLE;
        if ((ap_cap & IEEE80211_VHT_CAP_BEAMFORMEE_STS_MASK) <
            (cap & IEEE80211_VHT_CAP_BEAMFORMEE_STS_MASK))
            cap = (cap & ~IEEE80211_VHT_CAP_BEAMFORMEE_STS_MASK) |
                  (ap_cap & IEEE80211_VHT_CAP_BEAMFORMEE_STS_MASK);

        pos = skb_put_zero(skb, 2 + sizeof(*vht));
        *pos++ = WLAN_EID_VHT_CAPABILITY;
        *pos++ = sizeof(*vht);
        vht = (struct ieee80211_vht_cap *)pos;
        vht->vht_cap_info = cpu_to_le32(cap);
        memcpy(&vht->supp_mcs, &mlme.sband->vht_cap.vht_mcs, sizeof(vht->supp_mcs));
    }

    if (mlme.wmm)
        skb_put_data(skb, wmm_info, sizeof(wmm_info));

    mlme_tx_mgmt(skb);

    mlme.tries++;
    wiphy_delayed_work_queue(mlme.hw->wiphy, &mlme.timeout_work, MLME_ASSOC_TIMEOUT);
}

/* ieee80211_ht_cap_ie_to_sta_ht_cap(): what the AP can receive that we can send. */
static void mlme_set_sta_ht_cap(const struct ieee80211_ht_cap *ie)
{
    struct ieee80211_link_sta *link_sta = &mlme.sta->deflink;
    struct ieee80211_sta_ht_cap ht_cap, own;
    int i, max_tx_streams;
    u8 tx_mcs_set_cap;

    memset(&ht_cap, 0, sizeof(ht_cap));
    mlme_own_ht_cap(&own);
    if (!ie || !own.ht_supported)
        goto apply;

    ht_cap.ht_supported = true;
    ht_cap.cap = le16_to_cpu(ie->cap_info) &
        (own.cap | ~(IEEE80211_HT_CAP_LDPC_CODING | IEEE80211_HT_CAP_SUP_WIDTH_20_40 |
                     IEEE80211_HT_CAP_GRN_FLD | IEEE80211_HT_CAP_SGI_20 |
                     IEEE80211_HT_CAP_SGI_40 | IEEE80211_HT_CAP_DSSSCCK40));
    /* STBC is asymmetric: our TX needs their RX and the other way round */
    if (!(own.cap & IEEE80211_HT_CAP_TX_STBC))
        ht_cap.cap &= ~IEEE80211_HT_CAP_RX_STBC;
    if (!(own.cap & IEEE80211_HT_CAP_RX_STBC))
        ht_cap.cap &= ~IEEE80211_HT_CAP_TX_STBC;

    ht_cap.ampdu_factor = ie->ampdu_params_info & IEEE80211_HT_AMPDU_PARM_FACTOR;
    ht_cap.ampdu_density = (ie->ampdu_params_info & IEEE80211_HT_AMPDU_PARM_DENSITY) >> 2;

    tx_mcs_set_cap = own.mcs.tx_params;
    ht_cap.mcs.tx_params = ie->mcs.tx_params;
    if (!(tx_mcs_set_cap & IEEE80211_HT_MCS_TX_DEFINED))
        goto apply;

    if (tx_mcs_set_cap & IEEE80211_HT_MCS_TX_RX_DIFF)
        max_tx_streams = ((tx_mcs_set_cap & IEEE80211_HT_MCS_TX_MAX_STREAMS_MASK) >>
                          IEEE80211_HT_MCS_TX_MAX_STREAMS_SHIFT) + 1;
    else
        max_tx_streams = IEEE80211_HT_MCS_TX_MAX_STREAMS;

    for (i = 0; i < max_tx_streams; i++)
        ht_cap.mcs.rx_mask[i] = own.mcs.rx_mask[i] & ie->mcs.rx_mask[i];
    if (own.mcs.rx_mask[32 / 8] & ie->mcs.rx_mask[32 / 8] & 1)
        ht_cap.mcs.rx_mask[32 / 8] |= 1;
    ht_cap.mcs.rx_highest = ie->mcs.rx_highest;

    link_sta->agg.max_amsdu_len = ht_cap.cap & IEEE80211_HT_CAP_MAX_AMSDU ?
        IEEE80211_MAX_MPDU_LEN_HT_7935 : IEEE80211_MAX_MPDU_LEN_HT_3839;

apply:
    link_sta->ht_cap = ht_cap;
    link_sta->smps_mode = IEEE80211_SMPS_OFF;
}

/* ieee80211_vht_cap_ie_to_sta_vht_cap(): the same for 802.11ac. */
static void mlme_set_sta_vht_cap(const struct ieee80211_vht_cap *ie)
{
    struct ieee80211_link_sta *link_sta = &mlme.sta->deflink;
    const struct ieee80211_sta_vht_cap *own = &mlme.sband->vht_cap;
    struct ieee80211_sta_vht_cap *vht_cap = &link_sta->vht_cap;
    u32 cap_info;
    int i;

    memset(vht_cap, 0, sizeof(*vht_cap));
    if (!ie || !link_sta->ht_cap.ht_supported || !own->vht_supported)
        return;

    vht_cap->vht_supported = true;
    cap_info = le32_to_cpu(ie->vht_cap_info);

    /* some capabilities as they are */
    vht_cap->cap = cap_info &
        (IEEE80211_VHT_CAP_RXLDPC | IEEE80211_VHT_CAP_VHT_TXOP_PS | IEEE80211_VHT_CAP_HTC_VHT |
         IEEE80211_VHT_CAP_MAX_A_MPDU_LENGTH_EXPONENT_MASK |
         IEEE80211_VHT_CAP_VHT_LINK_ADAPTATION_VHT_UNSOL_MFB |
         IEEE80211_VHT_CAP_VHT_LINK_ADAPTATION_VHT_MRQ_MFB |
         IEEE80211_VHT_CAP_RX_ANTENNA_PATTERN | IEEE80211_VHT_CAP_TX_ANTENNA_PATTERN);
    vht_cap->cap |= min_t(u32, cap_info & IEEE80211_VHT_CAP_MAX_MPDU_MASK,
                          own->cap & IEEE80211_VHT_CAP_MAX_MPDU_MASK);

    /* some by what we can do (this card: no 160 MHz) */
    switch (own->cap & IEEE80211_VHT_CAP_SUPP_CHAN_WIDTH_MASK) {
    case IEEE80211_VHT_CAP_SUPP_CHAN_WIDTH_160MHZ:
        vht_cap->cap |= cap_info & IEEE80211_VHT_CAP_SUPP_CHAN_WIDTH_160MHZ;
        break;
    case IEEE80211_VHT_CAP_SUPP_CHAN_WIDTH_160_80PLUS80MHZ:
        vht_cap->cap |= cap_info & IEEE80211_VHT_CAP_SUPP_CHAN_WIDTH_MASK;
        break;
    }
    vht_cap->cap |= cap_info & own->cap &
                    (IEEE80211_VHT_CAP_SHORT_GI_80 | IEEE80211_VHT_CAP_SHORT_GI_160);
    if (own->cap & IEEE80211_VHT_CAP_SU_BEAMFORMEE_CAPABLE)
        vht_cap->cap |= cap_info & (IEEE80211_VHT_CAP_SU_BEAMFORMER_CAPABLE |
                                    IEEE80211_VHT_CAP_SOUNDING_DIMENSIONS_MASK);
    if (own->cap & IEEE80211_VHT_CAP_SU_BEAMFORMER_CAPABLE)
        vht_cap->cap |= cap_info & (IEEE80211_VHT_CAP_SU_BEAMFORMEE_CAPABLE |
                                    IEEE80211_VHT_CAP_BEAMFORMEE_STS_MASK);
    if (own->cap & IEEE80211_VHT_CAP_MU_BEAMFORMER_CAPABLE)
        vht_cap->cap |= cap_info & IEEE80211_VHT_CAP_MU_BEAMFORMEE_CAPABLE;
    if (own->cap & IEEE80211_VHT_CAP_MU_BEAMFORMEE_CAPABLE)
        vht_cap->cap |= cap_info & IEEE80211_VHT_CAP_MU_BEAMFORMER_CAPABLE;
    if (own->cap & IEEE80211_VHT_CAP_TXSTBC)
        vht_cap->cap |= cap_info & IEEE80211_VHT_CAP_RXSTBC_MASK;
    if (own->cap & IEEE80211_VHT_CAP_RXSTBC_MASK)
        vht_cap->cap |= cap_info & IEEE80211_VHT_CAP_TXSTBC;

    memcpy(&vht_cap->vht_mcs, &ie->supp_mcs, sizeof(vht_cap->vht_mcs));
    if (!ieee80211_hw_check(mlme.hw, SUPPORTS_VHT_EXT_NSS_BW))
        vht_cap->vht_mcs.tx_highest &= ~cpu_to_le16(IEEE80211_VHT_EXT_NSS_BW_CAPABLE);
    else
        vht_cap->cap |= cap_info & IEEE80211_VHT_CAP_EXT_NSS_BW_MASK;

    /* per stream, the rates one side sends and the other receives */
    for (i = 0; i < 8; i++) {
        u16 own_rx = (le16_to_cpu(own->vht_mcs.rx_mcs_map) >> i * 2) & 3;
        u16 own_tx = (le16_to_cpu(own->vht_mcs.tx_mcs_map) >> i * 2) & 3;
        u16 peer_rx = (le16_to_cpu(vht_cap->vht_mcs.rx_mcs_map) >> i * 2) & 3;
        u16 peer_tx = (le16_to_cpu(vht_cap->vht_mcs.tx_mcs_map) >> i * 2) & 3;

        if (peer_tx != IEEE80211_VHT_MCS_NOT_SUPPORTED) {
            if (own_rx == IEEE80211_VHT_MCS_NOT_SUPPORTED)
                peer_tx = IEEE80211_VHT_MCS_NOT_SUPPORTED;
            else if (own_rx < peer_tx)
                peer_tx = own_rx;
        }
        if (peer_rx != IEEE80211_VHT_MCS_NOT_SUPPORTED) {
            if (own_tx == IEEE80211_VHT_MCS_NOT_SUPPORTED)
                peer_rx = IEEE80211_VHT_MCS_NOT_SUPPORTED;
            else if (own_tx < peer_rx)
                peer_rx = own_tx;
        }
        vht_cap->vht_mcs.rx_mcs_map &= ~cpu_to_le16(IEEE80211_VHT_MCS_NOT_SUPPORTED << i * 2);
        vht_cap->vht_mcs.rx_mcs_map |= cpu_to_le16(peer_rx << i * 2);
        vht_cap->vht_mcs.tx_mcs_map &= ~cpu_to_le16(IEEE80211_VHT_MCS_NOT_SUPPORTED << i * 2);
        vht_cap->vht_mcs.tx_mcs_map |= cpu_to_le16(peer_tx << i * 2);
    }

    /* a station claiming no rate at all on any stream is broken */
    if (vht_cap->vht_mcs.rx_mcs_map == cpu_to_le16(0xffff)) {
        vht_cap->vht_supported = false;
        return;
    }

    switch (vht_cap->cap & IEEE80211_VHT_CAP_MAX_MPDU_MASK) {
    case IEEE80211_VHT_CAP_MAX_MPDU_LENGTH_11454:
        link_sta->agg.max_amsdu_len = IEEE80211_MAX_MPDU_LEN_VHT_11454;
        break;
    case IEEE80211_VHT_CAP_MAX_MPDU_LENGTH_7991:
        link_sta->agg.max_amsdu_len = IEEE80211_MAX_MPDU_LEN_VHT_7991;
        break;
    default:
        link_sta->agg.max_amsdu_len = IEEE80211_MAX_MPDU_LEN_VHT_3895;
        break;
    }
}

/* ieee80211_sta_init_nss_bw_capa(): how many streams and how wide a channel
 * we may use towards the AP. */
static void mlme_set_sta_nss_bw(void)
{
    struct ieee80211_link_sta *link_sta = &mlme.sta->deflink;
    unsigned int cap_mhz = 20, mhz;
    int i;

    link_sta->rx_nss = 1;
    for (i = 0; link_sta->ht_cap.ht_supported && i < 4; i++)
        if (link_sta->ht_cap.mcs.rx_mask[i])
            link_sta->rx_nss = (u8)(i + 1);
    for (i = 7; link_sta->vht_cap.vht_supported && i >= 0; i--) {
        if (((le16_to_cpu(link_sta->vht_cap.vht_mcs.rx_mcs_map) >> i * 2) & 3) !=
            IEEE80211_VHT_MCS_NOT_SUPPORTED) {
            if (i + 1 > link_sta->rx_nss)
                link_sta->rx_nss = (u8)(i + 1);
            break;
        }
    }

    if (link_sta->vht_cap.vht_supported)
        cap_mhz = 80;
    else if (link_sta->ht_cap.cap & IEEE80211_HT_CAP_SUP_WIDTH_20_40)
        cap_mhz = 40;
    mhz = min(cap_mhz, mlme_chandef_mhz(&mlme.chandef));
    link_sta->bandwidth = mhz >= 80 ? IEEE80211_STA_RX_BW_80 :
                          mhz >= 40 ? IEEE80211_STA_RX_BW_40 : IEEE80211_STA_RX_BW_20;

    ieee80211_sta_recalc_aggregates(mlme.sta);
}

/* EDCA parameters per access category (ieee80211_sta_wmm_params / set_wmm_default). */
static void mlme_conf_tx(const struct element *wmm)
{
    /* WMM ACI (BE, BK, VI, VO) to mac80211 AC index */
    static const u8 aci_to_ac[4] = {
        IEEE80211_AC_BE, IEEE80211_AC_BK, IEEE80211_AC_VI, IEEE80211_AC_VO,
    };
    struct ieee80211_tx_queue_params params[IEEE80211_NUM_ACS];
    int ac, i;

    /* Without WMM every queue uses the legacy DCF parameters. */
    for (ac = 0; ac < IEEE80211_NUM_ACS; ac++) {
        memset(&params[ac], 0, sizeof(params[ac]));
        params[ac].aifs = 2;
        params[ac].cw_min = mlme.sband->band == NL80211_BAND_2GHZ &&
                            !(mlme.sta->deflink.supp_rates[NL80211_BAND_2GHZ] & ~0xfU) ? 31 : 15;
        params[ac].cw_max = 1023;
    }

    if (wmm) {
        const struct ieee80211_wmm_param_ie *ie = (const void *)wmm;

        for (i = 0; i < 4; i++) {
            const struct ieee80211_wmm_ac_param *p = &ie->ac[i];
            u8 aci = (p->aci_aifsn >> 5) & 3;

            ac = aci_to_ac[aci];
            params[ac].aifs = p->aci_aifsn & 0x0f;
            /* a client must not use an AIFSN below 2 */
            if (params[ac].aifs < 2)
                params[ac].aifs = 2;
            params[ac].cw_max = ecw2cw((p->cw & 0xf0) >> 4);
            params[ac].cw_min = ecw2cw(p->cw & 0x0f);
            params[ac].txop = get_unaligned_le16(&p->txop_limit);
            params[ac].acm = (p->aci_aifsn >> 4) & 1;
        }
    }

    for (ac = 0; ac < IEEE80211_NUM_ACS; ac++)
        if (mlme.local->ops->conf_tx)
            mlme.local->ops->conf_tx(mlme.hw, mlme.vif, 0, ac, &params[ac]);
}

static void mlme_rx_assoc_resp(const struct ieee80211_mgmt *mgmt, size_t len)
{
    const size_t fixed = offsetof(struct ieee80211_mgmt, u.assoc_resp.variable);
    const struct ieee80211_ops *ops = mlme.local->ops;
    struct ieee80211_bss_conf *conf = &mlme.vif->bss_conf;
    const struct element *elem, *wmm;
    const u8 *ies;
    size_t ies_len;
    u32 rates, basic;
    u16 capab, status, aid;
    u64 changed;

    if (mlme.state != RTW89_MLME_ASSOCIATING || len < fixed)
        return;

    wiphy_delayed_work_cancel(mlme.hw->wiphy, &mlme.timeout_work);

    capab = le16_to_cpu(mgmt->u.assoc_resp.capab_info);
    status = le16_to_cpu(mgmt->u.assoc_resp.status_code);
    aid = le16_to_cpu(mgmt->u.assoc_resp.aid) & ~(BIT(15) | BIT(14));
    ies = mgmt->u.assoc_resp.variable;
    ies_len = len - fixed;

    if (status != WLAN_STATUS_SUCCESS) {
        mlme_fail(-1000 - status, "the AP refused the association");
        return;
    }

    /* ieee80211_assoc_config_link(): the station entry for the AP */
    mlme_parse_rates(mlme_find_elem(WLAN_EID_SUPP_RATES, ies, ies_len) ? ies : mlme.bss.ies,
                     mlme_find_elem(WLAN_EID_SUPP_RATES, ies, ies_len) ? ies_len : mlme.bss.ies_len,
                     &rates, &basic);
    if (rates)
        mlme.sta->deflink.supp_rates[mlme.sband->band] = rates;

    wmm = mlme.wmm ? mlme_wmm_param(ies, ies_len) : NULL;
    if (mlme.wmm && !wmm)
        wmm = mlme_wmm_param(mlme.bss.ies, mlme.bss.ies_len);

    elem = mlme.ht ? mlme_elem(WLAN_EID_HT_CAPABILITY, ies, ies_len) : NULL;
    mlme_set_sta_ht_cap(elem && elem->datalen >= sizeof(struct ieee80211_ht_cap) ?
                        (const void *)elem->data : NULL);
    elem = mlme.vht ? mlme_elem(WLAN_EID_VHT_CAPABILITY, ies, ies_len) : NULL;
    mlme_set_sta_vht_cap(elem && elem->datalen >= sizeof(struct ieee80211_vht_cap) ?
                         (const void *)elem->data : NULL);
    mlme_set_sta_nss_bw();

    mlme.sta->aid = aid;
    mlme.sta->wme = wmm != NULL;
    mlme.sta->max_rx_aggregation_subframes = mlme.hw->max_rx_aggregation_subframes;
    mlme.aid = aid;

    /* the link: ERP, HT operation, QoS, DTIM */
    conf->assoc_capability = capab;
    elem = mlme_elem(WLAN_EID_ERP_INFO, ies, ies_len);
    conf->use_cts_prot = elem && elem->datalen >= 1 && (elem->data[0] & WLAN_ERP_USE_PROTECTION);
    conf->use_short_preamble = (capab & WLAN_CAPABILITY_SHORT_PREAMBLE) &&
        !(elem && elem->datalen >= 1 && (elem->data[0] & WLAN_ERP_BARKER_PREAMBLE));
    conf->use_short_slot = mlme.sband->band != NL80211_BAND_2GHZ ||
                           (capab & WLAN_CAPABILITY_SHORT_SLOT_TIME);
    elem = mlme.ht ? mlme_elem(WLAN_EID_HT_OPERATION, ies, ies_len) : NULL;
    conf->ht_operation_mode = elem && elem->datalen >= sizeof(struct ieee80211_ht_operation) ?
        le16_to_cpu(((const struct ieee80211_ht_operation *)elem->data)->operation_mode) : 0;
    conf->qos = wmm != NULL;
    elem = mlme_find_elem(WLAN_EID_TIM, mlme.bss.ies, mlme.bss.ies_len);
    conf->dtim_period = elem && elem->datalen >= 2 && elem->data[1] ? elem->data[1] : 1;
    changed = BSS_CHANGED_ERP_CTS_PROT | BSS_CHANGED_ERP_PREAMBLE | BSS_CHANGED_ERP_SLOT |
              BSS_CHANGED_HT | BSS_CHANGED_BASIC_RATES | BSS_CHANGED_QOS |
              BSS_CHANGED_BEACON_INFO;

    mlme_conf_tx(wmm);

    /* ieee80211_assoc_success(): the station is associated; without RSN it
     * may pass data straight away */
    if (mlme_sta_move(IEEE80211_STA_ASSOC)) {
        mlme_fail(-EIO, "association failed in the driver");
        return;
    }
    if (!mlme.rsn && mlme_sta_move(IEEE80211_STA_AUTHORIZED)) {
        mlme_fail(-EIO, "association failed in the driver");
        return;
    }

    /* ieee80211_set_associated(): tell the driver, interface level first */
    mlme.vif->cfg.assoc = true;
    mlme.vif->cfg.aid = aid;
    if (ops->vif_cfg_changed)
        ops->vif_cfg_changed(mlme.hw, mlme.vif, BSS_CHANGED_ASSOC);
    if (ops->link_info_changed)
        ops->link_info_changed(mlme.hw, mlme.vif, conf, changed);

    mlme.last_error = 0;
    if (!mlme.rsn)
        rtw89_data_authorize();
    mlme_set_state(mlme.rsn ? RTW89_MLME_ASSOCIATED : RTW89_MLME_CONNECTED);
    /* an AP that never starts or finishes the handshake must not leave us
     * associated without keys for ever */
    if (mlme.rsn)
        wiphy_delayed_work_queue(mlme.hw->wiphy, &mlme.timeout_work, MLME_KEY_TIMEOUT);
    mlme_info("associated with %02x:%02x:%02x:%02x:%02x:%02x on %u MHz, AID %u, %s%s%s",
              mlme.bss.bssid[0], mlme.bss.bssid[1], mlme.bss.bssid[2],
              mlme.bss.bssid[3], mlme.bss.bssid[4], mlme.bss.bssid[5],
              mlme.bss.freq, aid, mlme.sta->deflink.ht_cap.ht_supported ? "HT" : "legacy",
              wmm ? ", WMM" : "", mlme.rsn ? ", waiting for the key handshake" : "");
}

static void mlme_rx_auth(const struct ieee80211_mgmt *mgmt, size_t len)
{
    u16 alg, transaction, status;

    if (mlme.state != RTW89_MLME_AUTHENTICATING ||
        len < offsetof(struct ieee80211_mgmt, u.auth.variable))
        return;

    alg = le16_to_cpu(mgmt->u.auth.auth_alg);
    transaction = le16_to_cpu(mgmt->u.auth.auth_transaction);
    status = le16_to_cpu(mgmt->u.auth.status_code);
    if (alg != WLAN_AUTH_OPEN || transaction != 2)
        return;

    wiphy_delayed_work_cancel(mlme.hw->wiphy, &mlme.timeout_work);

    if (status != WLAN_STATUS_SUCCESS) {
        mlme_fail(-1000 - status, "the AP refused authentication");
        return;
    }

    /* ieee80211_mark_sta_auth() */
    if (mlme_sta_move(IEEE80211_STA_AUTH)) {
        mlme_fail(-EIO, "authentication failed in the driver");
        return;
    }

    mlme.tries = 0;
    mlme_set_state(RTW89_MLME_ASSOCIATING);
    mlme_send_assoc();
}

/* ------------------------------------------------------------------ */
/*  Received frames and timeouts                                        */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  WPA2 key handshakes (supplicant side)                               */
/* ------------------------------------------------------------------ */

/* EAPOL-Key frame: 4-byte EAPOL header, then the key descriptor. */
#define EAPOL_HDR_LEN           4
#define EAPOL_KEY_FIXED_LEN     95      /* descriptor up to and including key data length */
#define EAPOL_TYPE_KEY          3
#define EAPOL_KEY_DESC_RSN      2

#define KEY_OFF_INFO            1
#define KEY_OFF_KEY_LEN         3
#define KEY_OFF_REPLAY          5
#define KEY_OFF_NONCE           13
#define KEY_OFF_RSC             61
#define KEY_OFF_MIC             77
#define KEY_OFF_DATA_LEN        93

#define KEY_INFO_VERSION        0x0007
#define KEY_INFO_VER_SHA1_AES   2
#define KEY_INFO_PAIRWISE       BIT(3)
#define KEY_INFO_INSTALL        BIT(6)
#define KEY_INFO_ACK            BIT(7)
#define KEY_INFO_MIC            BIT(8)
#define KEY_INFO_SECURE         BIT(9)
#define KEY_INFO_ERROR          BIT(10)
#define KEY_INFO_REQUEST        BIT(11)
#define KEY_INFO_ENCRYPTED      BIT(12)

static const u8 mlme_eapol_llc[8] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8e };

/* Send an EAPOL frame to the AP. It takes the data path like any other frame
 * (and so is encrypted once there is a pairwise key), but is let through
 * before the port is authorised: what the control port does in mac80211. */
static void mlme_tx_eapol(const u8 *eapol, size_t len)
{
    struct sk_buff *skb = rtw89_data_tx_alloc(ETH_HLEN + len);
    u8 *eth;

    if (!skb)
        return;
    eth = skb->data;
    memcpy(eth, mlme.bss.bssid, ETH_ALEN);
    memcpy(eth + ETH_ALEN, mlme.vif->addr, ETH_ALEN);
    put_unaligned_be16(ETH_P_PAE, eth + 2 * ETH_ALEN);
    memcpy(eth + ETH_HLEN, eapol, len);
    rtw89_data_tx(skb);
}

/* Build and send an EAPOL-Key reply with a valid MIC. */
static void mlme_send_eapol_key(const struct mlme_ptk *ptk, u8 version, u16 key_info,
                                const u8 *replay, const u8 *nonce,
                                const u8 *key_data, size_t key_data_len)
{
    u8 frame[EAPOL_HDR_LEN + EAPOL_KEY_FIXED_LEN + sizeof(mlme_rsn_ie)];
    u8 *k = frame + EAPOL_HDR_LEN;
    size_t len = EAPOL_HDR_LEN + EAPOL_KEY_FIXED_LEN + key_data_len;
    u8 mic[RTW89_SHA1_LEN];

    if (key_data_len > sizeof(mlme_rsn_ie))
        return;

    memset(frame, 0, sizeof(frame));
    frame[0] = version;
    frame[1] = EAPOL_TYPE_KEY;
    put_unaligned_be16((u16)(EAPOL_KEY_FIXED_LEN + key_data_len), frame + 2);
    k[0] = EAPOL_KEY_DESC_RSN;
    put_unaligned_be16(key_info | KEY_INFO_VER_SHA1_AES | KEY_INFO_MIC, k + KEY_OFF_INFO);
    memcpy(k + KEY_OFF_REPLAY, replay, 8);
    if (nonce)
        memcpy(k + KEY_OFF_NONCE, nonce, 32);
    put_unaligned_be16((u16)key_data_len, k + KEY_OFF_DATA_LEN);
    if (key_data_len)
        memcpy(k + EAPOL_KEY_FIXED_LEN, key_data, key_data_len);

    /* the MIC covers the whole frame with the MIC field zero */
    rtw89_hmac_sha1(ptk->kck, sizeof(ptk->kck), frame, len, mic);
    memcpy(k + KEY_OFF_MIC, mic, 16);

    mlme_tx_eapol(frame, len);
}

/* PTK = PRF-384(PMK, "Pairwise key expansion",
 *               min(AA, SPA) || max(AA, SPA) || min(ANonce, SNonce) || max(...)) */
static void mlme_derive_ptk(void)
{
    const u8 *aa = mlme.bss.bssid, *spa = mlme.vif->addr;
    u8 data[2 * ETH_ALEN + 64], out[48];

    if (memcmp(aa, spa, ETH_ALEN) < 0) {
        memcpy(data, aa, ETH_ALEN);
        memcpy(data + ETH_ALEN, spa, ETH_ALEN);
    } else {
        memcpy(data, spa, ETH_ALEN);
        memcpy(data + ETH_ALEN, aa, ETH_ALEN);
    }
    if (memcmp(mlme.anonce, mlme.snonce, 32) < 0) {
        memcpy(data + 12, mlme.anonce, 32);
        memcpy(data + 44, mlme.snonce, 32);
    } else {
        memcpy(data + 12, mlme.snonce, 32);
        memcpy(data + 44, mlme.anonce, 32);
    }

    rtw89_sha1_prf(mlme.pmk, sizeof(mlme.pmk), "Pairwise key expansion",
                   data, sizeof(data), out, sizeof(out));
    memcpy(mlme.tptk.kck, out, 16);
    memcpy(mlme.tptk.kek, out + 16, 16);
    memcpy(mlme.tptk.tk, out + 32, 16);
    memset(out, 0, sizeof(out));
    mlme.tptk_valid = true;
}

/*
 * Check the MIC of a received EAPOL-Key frame (@copy: the frame with the MIC
 * field zeroed). Message 1 is not authenticated, so the key derived from it
 * stays provisional until a frame verifies with it; a forged message 1 then
 * cannot replace the key of a working connection.
 */
static bool mlme_verify_mic(const u8 *copy, size_t len, const u8 *their_mic)
{
    u8 mic[RTW89_SHA1_LEN];

    if (mlme.tptk_valid) {
        rtw89_hmac_sha1(mlme.tptk.kck, sizeof(mlme.tptk.kck), copy, len, mic);
        if (rtw89_crypto_equal(mic, their_mic, 16)) {
            mlme.ptk = mlme.tptk;
            mlme.ptk_valid = true;
            memset(&mlme.tptk, 0, sizeof(mlme.tptk));
            mlme.tptk_valid = false;
            return true;
        }
    }
    if (mlme.ptk_valid) {
        rtw89_hmac_sha1(mlme.ptk.kck, sizeof(mlme.ptk.kck), copy, len, mic);
        if (rtw89_crypto_equal(mic, their_mic, 16))
            return true;
    }
    return false;
}

/* Find the group key in decrypted key data: a KDE dd <len> 00-0f-ac 01
 * <key id | tx> <reserved> <key>. Returns the key id, or -1. */
static int mlme_find_gtk(const u8 *kd, size_t len, const u8 **gtk)
{
    while (len >= 2) {
        u8 id = kd[0], elen = kd[1];

        if (id == 0xdd && elen == 0)    /* padding */
            break;
        if ((size_t)elen + 2 > len)
            break;
        if (id == 0xdd && elen == 6 + WLAN_KEY_LEN_CCMP &&
            kd[2] == 0x00 && kd[3] == 0x0f && kd[4] == 0xac && kd[5] == 1) {
            *gtk = kd + 8;
            return kd[6] & 0x03;
        }
        kd += elen + 2;
        len -= elen + 2;
    }
    return -1;
}

static void mlme_rx_eapol(const u8 *e, size_t len)
{
    const u8 *k = e + EAPOL_HDR_LEN, *kd, *gtk = NULL;
    u8 plain[256], copy[512];
    u16 key_info, body_len, kd_len;
    int gtk_idx = -1;
    u64 rsc;

    if (!mlme.eapol_rx++)
        mlme_info("the AP started the key handshake");

    if (len < EAPOL_HDR_LEN + EAPOL_KEY_FIXED_LEN || e[1] != EAPOL_TYPE_KEY)
        return;
    body_len = get_unaligned_be16(e + 2);
    if ((size_t)body_len + EAPOL_HDR_LEN > len || body_len < EAPOL_KEY_FIXED_LEN)
        return;
    len = (size_t)body_len + EAPOL_HDR_LEN;
    if (k[0] != EAPOL_KEY_DESC_RSN || len > sizeof(copy))
        return;

    key_info = get_unaligned_be16(k + KEY_OFF_INFO);
    kd_len = get_unaligned_be16(k + KEY_OFF_DATA_LEN);
    /* the packet number the AP has reached with the group key, low byte first */
    rsc = k[KEY_OFF_RSC] | (u64)k[KEY_OFF_RSC + 1] << 8 | (u64)k[KEY_OFF_RSC + 2] << 16 |
          (u64)k[KEY_OFF_RSC + 3] << 24 | (u64)k[KEY_OFF_RSC + 4] << 32 |
          (u64)k[KEY_OFF_RSC + 5] << 40;
    kd = k + EAPOL_KEY_FIXED_LEN;
    if ((size_t)kd_len + EAPOL_KEY_FIXED_LEN > body_len)
        return;

    if (!mlme.have_pmk || !mlme.sta || mlme.sta_state < IEEE80211_STA_ASSOC)
        return;
    if ((key_info & KEY_INFO_VERSION) != KEY_INFO_VER_SHA1_AES) {
        mlme_info("unsupported EAPOL-Key descriptor version %u", key_info & KEY_INFO_VERSION);
        return;
    }
    /* only frames an authenticator sends */
    if (!(key_info & KEY_INFO_ACK) || (key_info & (KEY_INFO_REQUEST | KEY_INFO_ERROR)))
        return;
    /* the replay counter must move forward */
    if (mlme.replay_valid && memcmp(k + KEY_OFF_REPLAY, mlme.replay, 8) <= 0)
        return;

    if (!(key_info & KEY_INFO_MIC)) {
        /* 4-way handshake, message 1: the AP's nonce */
        if (!(key_info & KEY_INFO_PAIRWISE))
            return;
        /* A new SNonce per handshake, not per message 1: the AP may already
         * be answering our first message 2 when its retry of message 1 arrives. */
        if (!mlme.snonce_valid || memcmp(mlme.anonce, k + KEY_OFF_NONCE, 32)) {
            memcpy(mlme.anonce, k + KEY_OFF_NONCE, 32);
            get_random_bytes(mlme.snonce, sizeof(mlme.snonce));
            mlme.snonce_valid = true;
            mlme_derive_ptk();
        }
        if (!mlme.tptk_valid)
            return;
        mlme_send_eapol_key(&mlme.tptk, e[0], KEY_INFO_PAIRWISE, k + KEY_OFF_REPLAY,
                            mlme.snonce, mlme_rsn_ie, sizeof(mlme_rsn_ie));
        return;
    }

    /* everything else is authenticated with the KCK */
    memcpy(copy, e, len);
    memset(copy + EAPOL_HDR_LEN + KEY_OFF_MIC, 0, 16);
    if (!mlme_verify_mic(copy, len, k + KEY_OFF_MIC)) {
        mlme_info("EAPOL-Key MIC mismatch (wrong password?)");
        return;
    }
    memcpy(mlme.replay, k + KEY_OFF_REPLAY, 8);
    mlme.replay_valid = true;

    if (key_info & KEY_INFO_ENCRYPTED) {
        if (kd_len < 24 || kd_len % 8 || kd_len - 8 > sizeof(plain) ||
            !rtw89_aes_unwrap(mlme.ptk.kek, kd, kd_len, plain)) {
            mlme_info("could not decrypt the EAPOL key data");
            return;
        }
        gtk_idx = mlme_find_gtk(plain, kd_len - 8, &gtk);
    }

    if (key_info & KEY_INFO_PAIRWISE) {
        /* 4-way handshake, message 3 */
        if (memcmp(k + KEY_OFF_NONCE, mlme.anonce, 32)) {
            mlme_info("handshake message 3 carries a different ANonce");
            goto out;
        }
        if (gtk_idx < 0) {
            mlme_info("handshake message 3 has no group key");
            goto out;
        }

        mlme_send_eapol_key(&mlme.ptk, e[0], KEY_INFO_PAIRWISE | KEY_INFO_SECURE,
                            k + KEY_OFF_REPLAY, NULL, NULL, 0);

        /*
         * Keys go in after message 4 is on its way, which is sent in the clear.
         * A repeated message 3 (our message 4 was lost) must not install the
         * same key again: that would restart its packet numbers, and reusing
         * a packet number breaks CCMP (the KRACK attack).
         */
        if ((key_info & KEY_INFO_INSTALL) && !mlme_key_is(mlme.ptk_conf, mlme.ptk.tk, 0)) {
            /* a renewed key: data waits until the new one is in */
            rtw89_data_set_tx_key(NULL);
            rtw89_data_set_rx_key(-1, false, 0);
            mlme_key_remove(&mlme.ptk_conf, mlme.sta);
            if (mlme_key_install(&mlme.ptk_conf, mlme.sta, mlme.ptk.tk, 0)) {
                mlme_fail(-EIO, "the driver refused the pairwise key");
                goto out;
            }
            rtw89_data_set_rx_key(-1, true, 0);
            rtw89_data_set_tx_key(mlme.ptk_conf);
            if (mlme.sta_state == IEEE80211_STA_AUTHORIZED)
                mlme_info("pairwise key renewed");
        }
        if (mlme_gtk_install(gtk_idx, gtk, rsc)) {
            mlme_fail(-EIO, "the driver refused the group key");
            goto out;
        }
        mlme.snonce_valid = false;      /* the next handshake gets a new one */

        if (mlme.sta_state < IEEE80211_STA_AUTHORIZED) {
            if (mlme_sta_move(IEEE80211_STA_AUTHORIZED)) {
                mlme_fail(-EIO, "authorising the station failed in the driver");
                goto out;
            }
            wiphy_delayed_work_cancel(mlme.hw->wiphy, &mlme.timeout_work);
            rtw89_data_authorize();
            mlme_set_state(RTW89_MLME_CONNECTED);
            mlme_info("WPA2 handshake complete: keys installed, connected");
        }
    } else {
        /* group key handshake, message 1: a new group key */
        if (gtk_idx < 0 || !mlme.ptk_conf)
            goto out;
        mlme_send_eapol_key(&mlme.ptk, e[0], KEY_INFO_SECURE, k + KEY_OFF_REPLAY,
                            NULL, NULL, 0);
        if (mlme_key_is(mlme.gtk_conf[gtk_idx], gtk, (s8)gtk_idx))
            goto out;
        if (mlme_gtk_install(gtk_idx, gtk, rsc))
            mlme_info("the driver refused the new group key");
        else
            mlme_info("group key renewed");
    }
out:
    memset(plain, 0, sizeof(plain));
}

/* ------------------------------------------------------------------ */
/*  Action frames                                                       */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  BlockAck sessions (net/mac80211/agg-rx.c, agg-tx.c)                 */
/* ------------------------------------------------------------------ */

static int mlme_ampdu_action(enum ieee80211_ampdu_mlme_action action, u16 tid, u16 ssn,
                             u16 buf_size, bool amsdu)
{
    struct ieee80211_ampdu_params params = {
        .action = action,
        .sta = mlme.sta,
        .tid = tid,
        .ssn = ssn,
        .buf_size = buf_size,
        .amsdu = amsdu,
    };

    if (!mlme.local->ops->ampdu_action)
        return -EOPNOTSUPP;
    return mlme.local->ops->ampdu_action(mlme.hw, mlme.vif, &params);
}

static void mlme_send_delba(u16 tid, bool initiator, u16 reason)
{
    const size_t len = IEEE80211_MIN_ACTION_SIZE(delba);
    struct sk_buff *skb = mlme_alloc_frame(len);
    struct ieee80211_mgmt *mgmt;

    if (!skb)
        return;
    mgmt = mlme_mgmt_header(skb, IEEE80211_STYPE_ACTION, len);
    mgmt->u.action.category = WLAN_CATEGORY_BACK;
    mgmt->u.action.action_code = WLAN_ACTION_DELBA;
    mgmt->u.action.delba.params = cpu_to_le16((u16)(tid << 12) |
                                              (initiator ? IEEE80211_DELBA_PARAM_INITIATOR_MASK : 0));
    mgmt->u.action.delba.reason_code = cpu_to_le16(reason);
    mlme_tx_mgmt(skb);
}

/* ---- receive side: the AP sends us aggregates ---- */

static void mlme_rx_ba_stop(u16 tid)
{
    if (tid >= IEEE80211_NUM_TIDS || !(mlme.rx_ba & BIT(tid)))
        return;
    mlme.rx_ba &= ~BIT(tid);
    rtw89_data_rx_ba_stop((u8)tid);
    mlme_ampdu_action(IEEE80211_AMPDU_RX_STOP, tid, 0, 0, false);
}

/* __ieee80211_start_rx_ba_session(): the AP asks to send us aggregates. */
static void mlme_rx_addba_req(const struct ieee80211_mgmt *req, size_t len)
{
    const size_t resp_len = IEEE80211_MIN_ACTION_SIZE(addba_resp);
    u16 capab, tid, buf_size, ssn, status = WLAN_STATUS_SUCCESS;
    struct ieee80211_mgmt *mgmt;
    struct sk_buff *skb;
    bool amsdu;

    if (len < IEEE80211_MIN_ACTION_SIZE(addba_req))
        return;
    capab = le16_to_cpu(req->u.action.addba_req.capab);
    tid = (capab & IEEE80211_ADDBA_PARAM_TID_MASK) >> 2;
    buf_size = (capab & IEEE80211_ADDBA_PARAM_BUF_SIZE_MASK) >> 6;
    ssn = le16_to_cpu(req->u.action.addba_req.start_seq_num) >> 4;
    amsdu = (capab & IEEE80211_ADDBA_PARAM_AMSDU_MASK) &&
            ieee80211_hw_check(mlme.hw, SUPPORTS_AMSDU_IN_AMPDU);

    /* zero means "as many as you can take" */
    if (!buf_size || buf_size > IEEE80211_MAX_AMPDU_BUF_HT)
        buf_size = IEEE80211_MAX_AMPDU_BUF_HT;
    if (buf_size > mlme.sta->max_rx_aggregation_subframes)
        buf_size = mlme.sta->max_rx_aggregation_subframes;

    if (!mlme.sta->deflink.ht_cap.ht_supported || !buf_size) {
        status = WLAN_STATUS_REQUEST_DECLINED;
    } else if (!(capab & IEEE80211_ADDBA_PARAM_POLICY_MASK)) {
        status = WLAN_STATUS_INVALID_QOS_PARAM;     /* delayed BlockAck: not supported */
    } else {
        /* a new request replaces the session it had */
        mlme_rx_ba_stop(tid);
        if (mlme_ampdu_action(IEEE80211_AMPDU_RX_START, tid, ssn, buf_size, amsdu) ||
            rtw89_data_rx_ba_start((u8)tid, ssn, buf_size))
            status = WLAN_STATUS_REQUEST_DECLINED;
        else
            mlme.rx_ba |= BIT(tid);
    }

    skb = mlme_alloc_frame(resp_len);
    if (!skb)
        return;
    mgmt = mlme_mgmt_header(skb, IEEE80211_STYPE_ACTION, resp_len);
    mgmt->u.action.category = WLAN_CATEGORY_BACK;
    mgmt->u.action.action_code = WLAN_ACTION_ADDBA_RESP;
    mgmt->u.action.addba_resp.dialog_token = req->u.action.addba_req.dialog_token;
    mgmt->u.action.addba_resp.status = cpu_to_le16(status);
    mgmt->u.action.addba_resp.capab =
        cpu_to_le16((amsdu ? IEEE80211_ADDBA_PARAM_AMSDU_MASK : 0) |
                    IEEE80211_ADDBA_PARAM_POLICY_MASK | (u16)(tid << 2) | (u16)(buf_size << 6));
    mgmt->u.action.addba_resp.timeout = req->u.action.addba_req.timeout;
    mlme_tx_mgmt(skb);
}

/* ---- transmit side: the driver wants to send aggregates ---- */

#define MLME_BA_NONE            0
#define MLME_BA_WAIT            1       /* request sent, the TID's frames held back */
#define MLME_BA_OPERATIONAL     2

#define MLME_ADDBA_TIMEOUT      HZ          /* ADDBA_RESP_INTERVAL */
#define MLME_BA_BURST_TRIES     3           /* HT_AGG_BURST_RETRIES */
#define MLME_BA_MAX_TRIES       15          /* HT_AGG_MAX_RETRIES */
#define MLME_BA_RETRY_PERIOD    (15 * HZ)   /* HT_AGG_RETRIES_PERIOD */

/* End (or give up on) the session; the TID's frames flow again, one by one. */
static void mlme_tx_ba_stop(u16 tid, bool send_delba, bool destroy)
{
    struct mlme_tx_ba *ba;

    if (tid >= ARRAY_SIZE(mlme.tx_ba))
        return;
    ba = &mlme.tx_ba[tid];
    if (ba->state == MLME_BA_NONE)
        return;

    mlme_ampdu_action(destroy ? IEEE80211_AMPDU_TX_STOP_FLUSH : IEEE80211_AMPDU_TX_STOP_CONT,
                      tid, 0, 0, false);
    if (send_delba)
        mlme_send_delba(tid, true, WLAN_REASON_QSTA_NOT_USE);
    ba->state = MLME_BA_NONE;
    rtw89_data_tx_ba_resume((u8)tid);
}

/* ieee80211_tx_ba_session_handle_start() */
static void mlme_tx_ba_start(u16 tid)
{
    const size_t len = IEEE80211_MIN_ACTION_SIZE(addba_req);
    struct mlme_tx_ba *ba = &mlme.tx_ba[tid];
    struct ieee80211_mgmt *mgmt;
    struct sk_buff *skb;

    if (mlme.state != RTW89_MLME_CONNECTED || ba->state != MLME_BA_NONE)
        return;

    if (rtw89_data_tx_ba_prepare((u8)tid, &ba->ssn))
        return;
    if (mlme_ampdu_action(IEEE80211_AMPDU_TX_START, tid, ba->ssn, 0, false) !=
        IEEE80211_AMPDU_TX_START_IMMEDIATE) {
        rtw89_data_tx_ba_resume((u8)tid);
        return;
    }

    ba->state = MLME_BA_WAIT;
    ba->tries++;
    ba->last_try = jiffies;
    ba->deadline = jiffies + MLME_ADDBA_TIMEOUT;
    if (!++mlme.dialog_token)
        mlme.dialog_token = 1;
    ba->dialog = mlme.dialog_token;

    skb = mlme_alloc_frame(len);
    if (skb) {
        mgmt = mlme_mgmt_header(skb, IEEE80211_STYPE_ACTION, len);
        mgmt->u.action.category = WLAN_CATEGORY_BACK;
        mgmt->u.action.action_code = WLAN_ACTION_ADDBA_REQ;
        mgmt->u.action.addba_req.dialog_token = ba->dialog;
        /* The size is the HT maximum whatever the driver will really send:
         * mac80211 does the same because some APs mishandle smaller ones. */
        mgmt->u.action.addba_req.capab =
            cpu_to_le16(IEEE80211_ADDBA_PARAM_AMSDU_MASK | IEEE80211_ADDBA_PARAM_POLICY_MASK |
                        (u16)(tid << 2) | (u16)(IEEE80211_MAX_AMPDU_BUF_HT << 6));
        mgmt->u.action.addba_req.timeout = 0;
        mgmt->u.action.addba_req.start_seq_num = cpu_to_le16((u16)(ba->ssn << 4));
        mlme_tx_mgmt(skb);
    }
    /* without an answer (or without the request) the timeout cleans up */
    wiphy_delayed_work_queue(mlme.hw->wiphy, &mlme.ba_timeout_work, MLME_ADDBA_TIMEOUT + 1);
}

/* ieee80211_process_addba_resp() */
static void mlme_rx_addba_resp(const struct ieee80211_mgmt *mgmt, size_t len)
{
    u16 capab, tid, buf_size;
    struct mlme_tx_ba *ba;

    if (len < IEEE80211_MIN_ACTION_SIZE(addba_resp))
        return;
    capab = le16_to_cpu(mgmt->u.action.addba_resp.capab);
    tid = (capab & IEEE80211_ADDBA_PARAM_TID_MASK) >> 2;
    buf_size = (capab & IEEE80211_ADDBA_PARAM_BUF_SIZE_MASK) >> 6;
    if (tid >= ARRAY_SIZE(mlme.tx_ba))
        return;
    ba = &mlme.tx_ba[tid];
    if (ba->state != MLME_BA_WAIT || mgmt->u.action.addba_resp.dialog_token != ba->dialog)
        return;

    if (le16_to_cpu(mgmt->u.action.addba_resp.status) != WLAN_STATUS_SUCCESS || !buf_size) {
        mlme_tx_ba_stop(tid, false, false);
        return;
    }

    buf_size = min_t(u16, buf_size, mlme.hw->max_tx_aggregation_subframes);
    mlme_ampdu_action(IEEE80211_AMPDU_TX_OPERATIONAL, tid, ba->ssn, buf_size,
                      capab & IEEE80211_ADDBA_PARAM_AMSDU_MASK);
    ba->state = MLME_BA_OPERATIONAL;
    ba->tries = 0;
    rtw89_data_tx_ba_resume((u8)tid);
}

/* ieee80211_process_delba() */
static void mlme_rx_delba(const struct ieee80211_mgmt *mgmt, size_t len)
{
    u16 params, tid;

    if (len < IEEE80211_MIN_ACTION_SIZE(delba))
        return;
    params = le16_to_cpu(mgmt->u.action.delba.params);
    tid = (params & IEEE80211_DELBA_PARAM_TID_MASK) >> 12;

    /* sent by the side that was sending aggregates, or by the receiving side */
    if (params & IEEE80211_DELBA_PARAM_INITIATOR_MASK)
        mlme_rx_ba_stop(tid);
    else
        mlme_tx_ba_stop(tid, false, false);
}

/* The AP did not answer an ADDBA request in time. */
static void mlme_ba_timeout_work(struct wiphy *wiphy, struct wiphy_work *work)
{
    unsigned long next = 0;
    u16 tid;

    if (!mlme.running)
        return;
    for (tid = 0; tid < ARRAY_SIZE(mlme.tx_ba); tid++) {
        struct mlme_tx_ba *ba = &mlme.tx_ba[tid];

        if (ba->state != MLME_BA_WAIT)
            continue;
        if (time_after(jiffies, ba->deadline))
            mlme_tx_ba_stop(tid, true, false);
        else if (!next || time_before(ba->deadline, next))
            next = ba->deadline;
    }
    if (next)
        wiphy_delayed_work_queue(wiphy, &mlme.ba_timeout_work, next - jiffies + 1);
}

/* What the driver's threads asked for (rtw89_mlme_tx_ba_request()). */
static void mlme_ba_work(struct wiphy *wiphy, struct wiphy_work *work)
{
    u16 tid;

    if (!mlme.running)
        return;
    for (tid = 0; tid < ARRAY_SIZE(mlme.tx_ba); tid++) {
        if (test_and_clear_bit(tid, &mlme.tx_ba_stop_req))
            mlme_tx_ba_stop(tid, true, false);
        if (test_and_clear_bit(tid, &mlme.tx_ba_start_req))
            mlme_tx_ba_start(tid);
    }
}

/*
 * ieee80211_start_tx_ba_session() / ieee80211_stop_tx_ba_session(): the driver
 * asks, from its own threads and with its own locks held. Only note the
 * request here; the work above does the rest under the wiphy mutex.
 */
int rtw89_mlme_tx_ba_request(struct ieee80211_sta *sta, u16 tid, bool start)
{
    struct mlme_tx_ba *ba;

    if (tid >= ARRAY_SIZE(mlme.tx_ba))
        return -EINVAL;
    if (!mlme.running || sta != mlme.sta || mlme.state != RTW89_MLME_CONNECTED)
        return -EAGAIN;
    ba = &mlme.tx_ba[tid];

    if (!start) {
        if (ba->state == MLME_BA_NONE)
            return -ENOENT;
        set_bit(tid, &mlme.tx_ba_stop_req);
    } else {
        /* -EINVAL makes rtw89 stop asking for this TID */
        if (!sta->deflink.ht_cap.ht_supported || !sta->wme)
            return -EINVAL;
        if (ba->state != MLME_BA_NONE)
            return -EAGAIN;
        /* an AP that keeps saying no is asked a few times, then rarely, then
         * not at all */
        if (ba->tries > MLME_BA_MAX_TRIES ||
            (ba->tries > MLME_BA_BURST_TRIES &&
             time_before(jiffies, ba->last_try + MLME_BA_RETRY_PERIOD)))
            return -EBUSY;
        set_bit(tid, &mlme.tx_ba_start_req);
    }
    wiphy_work_queue(mlme.hw->wiphy, &mlme.ba_work);
    return 0;
}

/* ieee80211_sta_tear_down_BA_sessions(): before the station goes away. */
static void mlme_ba_teardown(void)
{
    u16 tid;

    mlme.tx_ba_start_req = 0;
    mlme.tx_ba_stop_req = 0;
    wiphy_delayed_work_cancel(mlme.hw->wiphy, &mlme.ba_timeout_work);
    if (mlme.sta && mlme.sta_state >= IEEE80211_STA_ASSOC) {
        for (tid = 0; tid < ARRAY_SIZE(mlme.tx_ba); tid++)
            mlme_tx_ba_stop(tid, false, true);
        for (tid = 0; tid < IEEE80211_NUM_TIDS; tid++)
            mlme_rx_ba_stop(tid);
    }
    memset(mlme.tx_ba, 0, sizeof(mlme.tx_ba));
    mlme.rx_ba = 0;
}

static void mlme_rx_action(const struct ieee80211_mgmt *mgmt, size_t len)
{
    if (mlme.state < RTW89_MLME_ASSOCIATED || len < IEEE80211_MIN_ACTION_SIZE(action_code))
        return;
    /* with a pairwise key these would have to be protected frames (802.11w),
     * which this driver does not negotiate */
    if (mgmt->u.action.category != WLAN_CATEGORY_BACK)
        return;

    switch (mgmt->u.action.action_code) {
    case WLAN_ACTION_ADDBA_REQ:
        mlme_rx_addba_req(mgmt, len);
        break;
    case WLAN_ACTION_ADDBA_RESP:
        mlme_rx_addba_resp(mgmt, len);
        break;
    case WLAN_ACTION_DELBA:
        mlme_rx_delba(mgmt, len);
        break;
    }
}

static void mlme_rx_frame(struct sk_buff *skb)
{
    const struct ieee80211_mgmt *mgmt = (const void *)skb->data;
    __le16 fc = mgmt->frame_control;
    size_t len = skb->len;

    if (mlme.state == RTW89_MLME_IDLE || len < sizeof(struct ieee80211_hdr_3addr))
        return;

    if (!ieee80211_is_mgmt(fc) || !ether_addr_equal(mgmt->bssid, mlme.bss.bssid) ||
        !ether_addr_equal(mgmt->da, mlme.vif->addr))
        return;

    if (ieee80211_is_auth(fc)) {
        mlme_rx_auth(mgmt, len);
    } else if (ieee80211_is_assoc_resp(fc) || ieee80211_is_reassoc_resp(fc)) {
        mlme_rx_assoc_resp(mgmt, len);
    } else if (ieee80211_is_action(fc)) {
        mlme_rx_action(mgmt, len);
    } else if ((ieee80211_is_deauth(fc) || ieee80211_is_disassoc(fc)) &&
               len >= offsetof(struct ieee80211_mgmt, u.deauth) + 2) {
        u16 reason = le16_to_cpu(mgmt->u.deauth.reason_code);

        mlme.last_error = -2000 - reason;
        mlme_info("the AP sent %s, reason %u",
                  ieee80211_is_deauth(fc) ? "deauthentication" : "disassociation", reason);
        mlme_teardown(0);
    }
}

static void mlme_rx_work(struct wiphy *wiphy, struct wiphy_work *work)
{
    struct sk_buff *skb;

    while ((skb = skb_dequeue(&mlme.rxq))) {
        if (!mlme.running)
            ;
        else if (skb->protocol == htons(ETH_P_PAE))
            mlme_rx_eapol(skb->data, skb->len);
        else
            mlme_rx_frame(skb);
        kfree_skb(skb);
    }
}

/* An EAPOL frame from the AP, found by the data path. Any context: it is
 * queued behind the management frames that arrived before it. */
void rtw89_mlme_rx_eapol(const u8 *eapol, size_t len)
{
    struct sk_buff *skb;

    if (!mlme.running || mlme.state == RTW89_MLME_IDLE || skb_queue_len(&mlme.rxq) > 64)
        return;
    skb = dev_alloc_skb((u32)len);
    if (!skb)
        return;
    skb_put_data(skb, eapol, (u32)len);
    skb->protocol = htons(ETH_P_PAE);
    skb_queue_tail(&mlme.rxq, skb);
    wiphy_work_queue(mlme.hw->wiphy, &mlme.rx_work);
}

void rtw89_mlme_rx(struct sk_buff *skb)
{
    /* The driver leaves the FCS on the frame. */
    if (skb->len >= FCS_LEN)
        skb_trim(skb, skb->len - FCS_LEN);

    if (!mlme.running || mlme.state == RTW89_MLME_IDLE || skb_queue_len(&mlme.rxq) > 64) {
        kfree_skb(skb);
        return;
    }
    skb->protocol = 0;          /* an 802.11 frame, see mlme_rx_work() */
    skb_queue_tail(&mlme.rxq, skb);
    wiphy_work_queue(mlme.hw->wiphy, &mlme.rx_work);
}

/* The firmware stopped hearing the AP. Any context. */
void rtw89_mlme_connection_lost(void)
{
    if (mlme.running)
        wiphy_work_queue(mlme.hw->wiphy, &mlme.lost_work);
}

static void mlme_lost_work(struct wiphy *wiphy, struct wiphy_work *work)
{
    if (!mlme.running || mlme.state < RTW89_MLME_ASSOCIATED)
        return;
    mlme_info("lost the access point");
    mlme.last_error = -ENOLINK;
    mlme_teardown(WLAN_REASON_DISASSOC_DUE_TO_INACTIVITY);
}

static void mlme_timeout_work(struct wiphy *wiphy, struct wiphy_work *work)
{
    switch (mlme.state) {
    case RTW89_MLME_AUTHENTICATING:
        if (mlme.tries >= MLME_MAX_TRIES)
            mlme_fail(-ETIMEDOUT, "no answer to the authentication request");
        else
            mlme_send_auth();
        break;
    case RTW89_MLME_ASSOCIATING:
        if (mlme.tries >= MLME_MAX_TRIES)
            mlme_fail(-ETIMEDOUT, "no answer to the association request");
        else
            mlme_send_assoc();
        break;
    case RTW89_MLME_ASSOCIATED:
        mlme_info("the key handshake did not finish (%u EAPOL frame(s) from the AP)",
                  mlme.eapol_rx);
        mlme.last_error = -ETIMEDOUT;
        mlme_teardown(WLAN_REASON_4WAY_HANDSHAKE_TIMEOUT);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Commands                                                            */
/* ------------------------------------------------------------------ */

void rtw89_mlme_set_tx_tap(void (*tap)(const u8 *frame, size_t len))
{
    mlme.tx_tap = tap;
}

void rtw89_mlme_start(struct ieee80211_hw *hw, struct ieee80211_vif *vif, void (*notify)(void))
{
    void (*tap)(const u8 *, size_t) = mlme.tx_tap;

    memset(&mlme, 0, sizeof(mlme));
    mlme.tx_tap = tap;
    mlme.notify = notify;
    mlme.hw = hw;
    mlme.vif = vif;
    mlme.local = hw_to_local(hw);
    skb_queue_head_init(&mlme.rxq);
    wiphy_work_init(&mlme.rx_work, mlme_rx_work);
    wiphy_work_init(&mlme.lost_work, mlme_lost_work);
    wiphy_work_init(&mlme.ba_work, mlme_ba_work);
    wiphy_delayed_work_init(&mlme.ba_timeout_work, mlme_ba_timeout_work);
    wiphy_delayed_work_init(&mlme.timeout_work, mlme_timeout_work);
    mlme.running = true;
}

void rtw89_mlme_stop(void)
{
    if (!mlme.running)
        return;

    mlme_teardown(WLAN_REASON_DEAUTH_LEAVING);
    mlme.running = false;
    wiphy_delayed_work_cancel(mlme.hw->wiphy, &mlme.timeout_work);
    wiphy_work_cancel(mlme.hw->wiphy, &mlme.rx_work);
    wiphy_work_cancel(mlme.hw->wiphy, &mlme.lost_work);
    wiphy_work_cancel(mlme.hw->wiphy, &mlme.ba_work);
    wiphy_delayed_work_cancel(mlme.hw->wiphy, &mlme.ba_timeout_work);
    skb_queue_purge(&mlme.rxq);
    spin_lock_destroy(&mlme.rxq.lock);
    memset(mlme.pmk, 0, sizeof(mlme.pmk));
    mlme.have_pmk = false;
}

void rtw89_mlme_disconnect(u16 reason)
{
    if (!mlme.running || mlme.state == RTW89_MLME_IDLE)
        return;
    mlme_info("leaving the network");
    mlme_teardown(reason);
}

void rtw89_mlme_get_status(struct rtw89_mlme_status *status)
{
    memset(status, 0, sizeof(*status));
    status->state = mlme.running ? mlme.state : RTW89_MLME_IDLE;
    status->last_error = mlme.last_error;
    status->eapol_rx = mlme.eapol_rx;
    if (status->state != RTW89_MLME_IDLE) {
        status->width = (u8)mlme_chandef_mhz(&mlme.chandef);
        status->center_freq = (u16)mlme.chandef.center_freq1;
        status->mode = mlme.vht ? 2 : mlme.ht ? 1 : 0;
        status->nss = mlme.sta ? mlme.sta->deflink.rx_nss : 0;
    }
    if (status->state >= RTW89_MLME_ASSOCIATED) {
        unsigned int tid;

        status->rx_ba = mlme.rx_ba;
        for (tid = 0; tid < ARRAY_SIZE(mlme.tx_ba); tid++)
            if (mlme.tx_ba[tid].state == MLME_BA_OPERATIONAL)
                status->tx_ba |= BIT(tid);
    }
    if (status->state != RTW89_MLME_IDLE) {
        memcpy(status->bssid, mlme.bss.bssid, ETH_ALEN);
        memcpy(status->ssid, mlme.bss.ssid, mlme.bss.ssid_len);
        status->freq = mlme.bss.freq;
        status->aid = mlme.aid;
    }
}

/* ieee80211_mgd_auth() + ieee80211_prep_connection() */
int rtw89_mlme_connect(const struct rtw89_mlme_bss *bss, const u8 *pmk)
{
    const struct ieee80211_ops *ops = mlme.local->ops;
    struct ieee80211_bss_conf *conf = &mlme.vif->bss_conf;
    struct cfg80211_chan_def chandef;
    const struct element *elem;
    u32 rates, basic;
    int ret;

    if (!mlme.running)
        return -ENETDOWN;
    if (mlme.state != RTW89_MLME_IDLE)
        return -EBUSY;
    if (bss->ies_len > MLME_MAX_IES || !bss->ssid_len)
        return -EINVAL;

    mlme.bss = *bss;
    memcpy(mlme.ies, bss->ies, bss->ies_len);
    mlme.bss.ies = mlme.ies;
    mlme.last_error = 0;
    mlme.eapol_rx = 0;
    mlme.tries = 0;
    mlme.aid = 0;

    mlme.chan = ieee80211_get_channel(mlme.hw->wiphy, bss->freq);
    if (!mlme.chan || (mlme.chan->flags & IEEE80211_CHAN_DISABLED))
        return -EINVAL;
    mlme.sband = mlme.hw->wiphy->bands[mlme.chan->band];

    mlme.ht = mlme.sband->ht_cap.ht_supported &&
              mlme_find_elem(WLAN_EID_HT_CAPABILITY, mlme.ies, bss->ies_len) &&
              mlme_find_elem(WLAN_EID_HT_OPERATION, mlme.ies, bss->ies_len);
    mlme.wmm = mlme_wmm_param(mlme.ies, bss->ies_len) != NULL;
    /* 802.11n requires QoS */
    if (!mlme.wmm)
        mlme.ht = false;
    /* 802.11ac: 5 GHz only, on top of 802.11n */
    elem = mlme_find_elem(WLAN_EID_VHT_CAPABILITY, mlme.ies, bss->ies_len);
    mlme.vht = mlme.ht && mlme.sband->vht_cap.vht_supported &&
               elem && elem->datalen >= sizeof(struct ieee80211_vht_cap) &&
               mlme_find_elem(WLAN_EID_VHT_OPERATION, mlme.ies, bss->ies_len);
    mlme_determine_chandef(&mlme.chandef);

    elem = mlme_find_elem(WLAN_EID_RSN, mlme.ies, bss->ies_len);
    mlme.rsn = elem != NULL;
    mlme.have_pmk = false;
    if (elem) {
        ret = mlme_check_rsn(elem);
        if (ret)
            return ret;
        if (!pmk) {
            mlme_info("the network needs a password");
            return -EACCES;
        }
        memcpy(mlme.pmk, pmk, sizeof(mlme.pmk));
        mlme.have_pmk = true;
    } else if (bss->capability & WLAN_CAPABILITY_PRIVACY) {
        mlme_info("the network uses WEP or WPA1, which is not supported");
        return -EOPNOTSUPP;
    }

    mlme_parse_rates(mlme.ies, bss->ies_len, &rates, &basic);
    if (!rates) {
        mlme_info("the network advertises no rate this card supports");
        return -EINVAL;
    }
    if (!basic)
        basic = BIT(__ffs(rates));

    mlme_info("joining \"%.*s\" (%02x:%02x:%02x:%02x:%02x:%02x) on %u MHz, %u MHz wide%s%s",
              bss->ssid_len, bss->ssid, bss->bssid[0], bss->bssid[1], bss->bssid[2],
              bss->bssid[3], bss->bssid[4], bss->bssid[5], bss->freq,
              mlme_chandef_mhz(&mlme.chandef),
              mlme.vht ? ", 802.11ac" : mlme.ht ? ", 802.11n" : "",
              mlme.rsn ? ", WPA2" : ", open");

    /* the station entry for the AP, with what is known before association */
    mlme.sta = rtw89_m80211_sta_alloc(mlme.vif, bss->bssid);
    if (!mlme.sta)
        return -ENOMEM;
    mlme.sta_state = IEEE80211_STA_NOTEXIST;
    mlme.sta->deflink.supp_rates[mlme.sband->band] = rates;
    mlme.sta->deflink.rx_nss = 1;
    conf->basic_rates = basic;
    conf->beacon_int = bss->beacon_int;

    /* Frames from the AP are sorted from now on: the first handshake message
     * can arrive before the association response has been dealt with. Nothing
     * but the handshake passes until the port is authorised. */
    rtw89_data_attach(mlme.vif, mlme.sta, bss->bssid, mlme.chan->band, mlme.rsn);

    /* from here on mlme_teardown() cleans up */
    mlme_set_state(RTW89_MLME_AUTHENTICATING);

    /* ieee80211_prep_channel(): a channel context on the AP's channel */
    chandef = mlme.chandef;
    mlme.chanctx = kzalloc(sizeof(*mlme.chanctx) + mlme.hw->chanctx_data_size, GFP_KERNEL);
    if (!mlme.chanctx) {
        ret = -ENOMEM;
        goto err;
    }
    mlme.chanctx->def = chandef;
    mlme.chanctx->min_def = chandef;
    mlme.chanctx->rx_chains_static = 1;
    mlme.chanctx->rx_chains_dynamic = 1;
    ret = ops->add_chanctx(mlme.hw, mlme.chanctx);
    if (ret) {
        kfree(mlme.chanctx);
        mlme.chanctx = NULL;
        goto err;
    }
    conf->chanreq.oper = chandef;
    ret = ops->assign_vif_chanctx(mlme.hw, mlme.vif, conf, mlme.chanctx);
    if (ret)
        goto err;
    conf->chanctx_conf = mlme.chanctx;
    mlme.chanctx_assigned = true;

    /* tell the driver about BSSID, basic rates and timing */
    memcpy(to_mvif(mlme.vif)->bssid, bss->bssid, ETH_ALEN);
    memcpy(mlme.vif->cfg.ap_addr, bss->bssid, ETH_ALEN);
    memcpy(mlme.vif->cfg.ssid, bss->ssid, bss->ssid_len);
    mlme.vif->cfg.ssid_len = bss->ssid_len;
    if (ops->link_info_changed)
        ops->link_info_changed(mlme.hw, mlme.vif, conf,
                               BSS_CHANGED_BSSID | BSS_CHANGED_BASIC_RATES |
                               BSS_CHANGED_BEACON_INT);

    /* sta_info_insert() */
    ret = mlme_sta_move(IEEE80211_STA_NONE);
    if (ret)
        goto err;
    rtw89_m80211_sta_set_uploaded(mlme.sta, true);

    mlme_send_auth();
    return 0;

err:
    mlme.last_error = ret;
    mlme_teardown(0);
    return ret;
}
