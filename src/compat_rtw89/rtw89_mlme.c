// SPDX-License-Identifier: GPL-2.0
/*
 * Station-mode MLME: joining and leaving one network on one non-MLO station
 * interface. This is the part of net/mac80211/mlme.c the driver cannot do
 * without, and the order of driver calls follows it (ieee80211_prep_connection,
 * ieee80211_auth, ieee80211_send_assoc, ieee80211_assoc_success,
 * ieee80211_set_associated, ieee80211_set_disassoc).
 *
 * Scope so far: open-system authentication and association, 802.11n on a
 * 20 MHz channel, WPA2-PSK/CCMP announced in the association request. The key
 * handshake and the data path are not here yet; EAPOL frames from the AP are
 * only counted.
 *
 * Everything runs under the wiphy mutex, as in mac80211: commands take it,
 * received frames are queued and handled from a wiphy work.
 */
#include "rtw89_net80211.h"

#define MLME_AUTH_TIMEOUT   (HZ / 2)
#define MLME_ASSOC_TIMEOUT  (HZ / 2)
#define MLME_MAX_TRIES      3
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
    bool wmm;
    bool rsn;
    u8 group_cipher[4];         /* group data cipher suite from the AP's RSN IE */

    struct ieee80211_chanctx_conf *chanctx;
    bool chanctx_assigned;
    struct ieee80211_sta *sta;
    enum ieee80211_sta_state sta_state;
    u16 aid;

    struct sk_buff_head rxq;
    struct wiphy_work rx_work;
    struct wiphy_delayed_work timeout_work;
} mlme;

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

/* Undo whatever the join got to. @deauth_reason: tell the AP first if non-zero. */
static void mlme_teardown(u16 deauth_reason)
{
    const struct ieee80211_ops *ops = mlme.local->ops;
    struct ieee80211_bss_conf *conf = &mlme.vif->bss_conf;
    bool was_assoc = mlme.vif->cfg.assoc;
    u64 changed = BSS_CHANGED_BSSID;

    if (mlme.state == RTW89_MLME_IDLE)
        return;

    wiphy_delayed_work_cancel(mlme.hw->wiphy, &mlme.timeout_work);

    if (deauth_reason && mlme.sta && mlme.sta_state >= IEEE80211_STA_NONE)
        mlme_send_deauth(deauth_reason);

    /* clear the AP address only after the frames that need it are built */
    eth_zero_addr(to_mvif(mlme.vif)->bssid);
    eth_zero_addr(mlme.vif->cfg.ap_addr);
    mlme.vif->cfg.ssid_len = 0;

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

    mlme.state = RTW89_MLME_IDLE;
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
static void mlme_own_ht_cap(struct ieee80211_sta_ht_cap *own)
{
    *own = mlme.sband->ht_cap;
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
    if (mlme.rsn) {
        static const u8 rsn[] = {
            WLAN_EID_RSN, 20,
            1, 0,                       /* version */
            0x00, 0x0f, 0xac, 4,        /* group cipher: CCMP */
            1, 0, 0x00, 0x0f, 0xac, 4,  /* pairwise: CCMP */
            1, 0, 0x00, 0x0f, 0xac, 2,  /* AKM: PSK */
            0, 0,                       /* capabilities */
        };

        skb_put_data(skb, rsn, sizeof(rsn));
    }

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
    link_sta->bandwidth = IEEE80211_STA_RX_BW_20;

    /* ieee80211_sta_init_nss(): streams the AP can receive from us */
    link_sta->rx_nss = 1;
    for (i = 0; ht_cap.ht_supported && i < 4; i++)
        if (ht_cap.mcs.rx_mask[i])
            link_sta->rx_nss = i + 1;

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

    mlme.state = mlme.rsn ? RTW89_MLME_ASSOCIATED : RTW89_MLME_CONNECTED;
    mlme.last_error = 0;
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

    mlme.state = RTW89_MLME_ASSOCIATING;
    mlme.tries = 0;
    mlme_send_assoc();
}

/* ------------------------------------------------------------------ */
/*  Received frames and timeouts                                        */
/* ------------------------------------------------------------------ */

static void mlme_rx_data(const struct ieee80211_hdr *hdr, size_t len)
{
    static const u8 eapol_llc[8] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8e };
    unsigned int hdrlen = ieee80211_hdrlen(hdr->frame_control);

    if (!ieee80211_is_data_present(hdr->frame_control) ||
        ieee80211_has_protected(hdr->frame_control) ||
        len < hdrlen + sizeof(eapol_llc))
        return;
    if (memcmp((const u8 *)hdr + hdrlen, eapol_llc, sizeof(eapol_llc)))
        return;

    if (!mlme.eapol_rx++)
        mlme_info("the AP started the key handshake (EAPOL frame, %zu bytes)",
                  len - hdrlen - sizeof(eapol_llc));
}

static void mlme_rx_frame(struct sk_buff *skb)
{
    const struct ieee80211_mgmt *mgmt = (const void *)skb->data;
    __le16 fc = mgmt->frame_control;
    size_t len = skb->len;

    if (mlme.state == RTW89_MLME_IDLE || len < sizeof(struct ieee80211_hdr_3addr))
        return;

    if (ieee80211_is_data(fc)) {
        const struct ieee80211_hdr *hdr = (const void *)skb->data;

        /* from the AP to us: addr1 = us, addr2 = BSSID */
        if (ether_addr_equal(hdr->addr1, mlme.vif->addr) &&
            ether_addr_equal(hdr->addr2, mlme.bss.bssid))
            mlme_rx_data(hdr, len);
        return;
    }

    if (!ieee80211_is_mgmt(fc) || !ether_addr_equal(mgmt->bssid, mlme.bss.bssid) ||
        !ether_addr_equal(mgmt->da, mlme.vif->addr))
        return;

    if (ieee80211_is_auth(fc)) {
        mlme_rx_auth(mgmt, len);
    } else if (ieee80211_is_assoc_resp(fc) || ieee80211_is_reassoc_resp(fc)) {
        mlme_rx_assoc_resp(mgmt, len);
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
        if (mlme.running)
            mlme_rx_frame(skb);
        kfree_skb(skb);
    }
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
    skb_queue_tail(&mlme.rxq, skb);
    wiphy_work_queue(mlme.hw->wiphy, &mlme.rx_work);
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
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Commands                                                            */
/* ------------------------------------------------------------------ */

void rtw89_mlme_start(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
    memset(&mlme, 0, sizeof(mlme));
    mlme.hw = hw;
    mlme.vif = vif;
    mlme.local = hw_to_local(hw);
    skb_queue_head_init(&mlme.rxq);
    wiphy_work_init(&mlme.rx_work, mlme_rx_work);
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
    skb_queue_purge(&mlme.rxq);
    spin_lock_destroy(&mlme.rxq.lock);
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
        memcpy(status->bssid, mlme.bss.bssid, ETH_ALEN);
        memcpy(status->ssid, mlme.bss.ssid, mlme.bss.ssid_len);
        status->freq = mlme.bss.freq;
        status->aid = mlme.aid;
    }
}

/* ieee80211_mgd_auth() + ieee80211_prep_connection() */
int rtw89_mlme_connect(const struct rtw89_mlme_bss *bss)
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

    elem = mlme_find_elem(WLAN_EID_RSN, mlme.ies, bss->ies_len);
    mlme.rsn = elem != NULL;
    if (elem) {
        ret = mlme_check_rsn(elem);
        if (ret)
            return ret;
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

    mlme_info("joining \"%.*s\" (%02x:%02x:%02x:%02x:%02x:%02x) on %u MHz%s%s",
              bss->ssid_len, bss->ssid, bss->bssid[0], bss->bssid[1], bss->bssid[2],
              bss->bssid[3], bss->bssid[4], bss->bssid[5], bss->freq,
              mlme.ht ? ", 802.11n" : "", mlme.rsn ? ", WPA2" : ", open");

    /* the station entry for the AP, with what is known before association */
    mlme.sta = rtw89_m80211_sta_alloc(mlme.vif, bss->bssid);
    if (!mlme.sta)
        return -ENOMEM;
    mlme.sta_state = IEEE80211_STA_NOTEXIST;
    mlme.sta->deflink.supp_rates[mlme.sband->band] = rates;
    mlme.sta->deflink.rx_nss = 1;
    conf->basic_rates = basic;
    conf->beacon_int = bss->beacon_int;

    /* from here on mlme_teardown() cleans up */
    mlme.state = RTW89_MLME_AUTHENTICATING;

    /* ieee80211_prep_channel(): a channel context on the AP's channel */
    cfg80211_chandef_create(&chandef, mlme.chan,
                            mlme.ht ? NL80211_CHAN_HT20 : NL80211_CHAN_NO_HT);
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
