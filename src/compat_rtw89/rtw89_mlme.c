// SPDX-License-Identifier: GPL-2.0
/*
 * Station-mode MLME: joining and leaving one network on one non-MLO station
 * interface. This is the part of net/mac80211/mlme.c the driver cannot do
 * without, and the order of driver calls follows it (ieee80211_prep_connection,
 * ieee80211_auth, ieee80211_send_assoc, ieee80211_assoc_success,
 * ieee80211_set_associated, ieee80211_set_disassoc).
 *
 * Scope so far: open-system and SAE (WPA3-Personal) authentication and
 * association; 802.11n, 802.11ac and 802.11ax on channels up to 80 MHz wide;
 * WPA2-PSK, PSK-SHA256 and SAE with CCMP: the supplicant side of the 4-way and
 * group key handshakes (IEEE 802.11 12.7.6/12.7.7) and installing the keys in
 * the driver; protected management frames (802.11w), in software as
 * mac80211 does for these chips; BlockAck sessions.
 * Data frames are rtw89_data.c's. While connected: the AP is probed when the
 * driver reports missed beacons, and its beacons are watched for changed
 * parameters and for a move to another channel (which is answered by leaving;
 * the glue joins again).
 *
 * Everything runs under the wiphy mutex, as in mac80211: commands take it,
 * received frames are queued and handled from a wiphy work.
 */
#include "rtw89_net80211.h"
#include "rtw89_crypto.h"
#include "rtw89_sae.h"

#define MLME_AUTH_TIMEOUT   (HZ / 2)
#define MLME_ASSOC_TIMEOUT  (HZ / 2)
#define MLME_MAX_TRIES      3
#define MLME_KEY_TIMEOUT    (10 * HZ)   /* for the whole key handshake */
#define MLME_MAX_IES        1024
#define MLME_PROBE_WAIT     (HZ / 2)    /* for a probing frame's fate (probe_wait_ms) */
#define MLME_PROBE_TRIES    2           /* max_nullfunc_tries */
#define MLME_SAE_TIMEOUT    HZ          /* for each SAE message: the AP computes too */
#define MLME_SA_QUERY_WAIT  (HZ / 2)    /* for the AP's answer to an SA Query */
#define MLME_COMEBACK_MAX   (2 * HZ)    /* longest "come back later" waited for */

/* RSN key management suites, 00-0f-ac:<n> */
#define AKM_8021X           1
#define AKM_PSK             2
#define AKM_PSK_SHA256      6
#define AKM_SAE             8

#define RSN_CAP_MFPR        BIT(6)      /* management frame protection required */
#define RSN_CAP_MFPC        BIT(7)      /* ... capable */

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
    bool he;                    /* ... and 802.11ax */
    unsigned int bw_limit;      /* MHz: the widest channel we may claim to the AP */
    struct cfg80211_chan_def chandef;   /* the channel and width we operate on */
    bool wmm;
    bool rsn;
    /* The key handshake is somebody else's (macOS's supplicant, for networks
     * with 802.1X sign-in): EAPOL frames pass through the data path and the
     * keys arrive through rtw89_mlme_set_key(). */
    bool external;
    u8 ext_rsn_ie[64];          /* the RSN element to associate with */
    u8 ext_rsn_len;
    u8 held_m1[160];            /* handshake message 1 that came before the PMK */
    u16 held_m1_len;
    u8 group_cipher[4];         /* group data cipher suite from the AP's RSN IE */

    /* what the AP's RSN element offers, and what this join takes of it */
    u32 ap_akms;                /* BIT(n) for each 00-0f-ac:n suite */
    u16 ap_rsn_caps;
    u8 akm;                     /* AKM_* in use */
    u8 key_ver;                 /* EAPOL-Key descriptor version: 2, 3 or 0 */
    bool mfp;                   /* management frame protection (802.11w) */
    u8 own_rsn_ie[2 + 20];      /* the RSN element of the association request */
    u8 own_rsn_len;

    /* WPA3-Personal: SAE authentication (12.4) */
    struct rtw89_sae *sae;
    bool sae_confirm_sent;      /* own confirm out: waiting for the AP's */
    bool sae_fallback;          /* the network takes WPA2 too: SAE may fail over to it */
    u8 sae_token[64];           /* anti-clogging token the AP asked for */
    u8 sae_token_len;

    /* 802.11w: robust management frames from the AP, and SA Query (11.13) */
    u64 mgmt_rx_pn;
    bool mgmt_rx_pn_valid;
    bool sa_query_pending;
    u8 sa_query_id[WLAN_SA_QUERY_TR_ID_LEN];
    u16 sa_query_reason;        /* of the unprotected frame that started it */
    struct wiphy_delayed_work sa_query_work;

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

    /* connection monitor (ieee80211_mgd_probe_ap()) */
    bool poll;                  /* probing the AP after missed beacons */
    u8 probe_tries;             /* null frames sent in this round; 0 once one was acked */
    bool probe_failed;          /* the last one was not acknowledged */
    unsigned long probe_timeout;
    struct wiphy_work beacon_loss_work;
    struct wiphy_work probe_work;
    struct wiphy_delayed_work probe_timeout_work;
    struct wiphy_delayed_work bcn_mon_work;     /* see mlme_bcn_mon_work() */
    unsigned long last_beacon;                  /* jiffies of the AP's latest beacon */
    u32 eapol_reports;                          /* handshake replies reported on */

    /* what the AP's beacons said last */
    u32 beacon_hash;
    bool beacon_hash_valid;
    struct cfg80211_chan_def beacon_def;    /* the channel by the first beacon */
    bool beacon_def_valid;
    int wmm_param_set, mu_edca_param_set;   /* parameter set counts in use, -1: none */
    bool csa_pending;                       /* the AP announced a channel switch */
    struct wiphy_delayed_work csa_work;
    u32 beacons, beacon_losses, beacon_updates, probe_acks;

    void (*notify)(void);
    void (*tx_tap)(const u8 *frame, size_t len);

    struct sk_buff_head rxq;
    struct wiphy_work rx_work;
    struct wiphy_work lost_work;
    struct wiphy_delayed_work timeout_work;
} mlme;

static void mlme_set_state(enum rtw89_mlme_state state)
{
    mlme.state = state;
    if (mlme.notify)
        mlme.notify();
}

/* ... and what we send when the sign-in is 802.1X and nobody gave us one */
static const u8 mlme_rsn_ie_8021x[] = {
    WLAN_EID_RSN, 20,
    1, 0,
    0x00, 0x0f, 0xac, 4,
    1, 0, 0x00, 0x0f, 0xac, 4,
    1, 0, 0x00, 0x0f, 0xac, 1,  /* AKM: 802.1X */
    0, 0,
};

#define mlme_info(fmt, ...) IOLog("[rtw89 mlme] " fmt "\n", ##__VA_ARGS__)

static void mlme_bcn_mon_start(void);

/* Not in mlme: it holds across stopping and starting the radio. */
static bool mlme_he_off;

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

/* The payload of an extension element (after the extension id), if there is
 * one of at least @min bytes. */
static const u8 *mlme_ext_elem(u8 ext_eid, const u8 *ies, size_t len, size_t min, u8 *out_len)
{
    const struct element *elem = ies ? cfg80211_find_ext_elem(ext_eid, ies, len) : NULL;

    if (!elem || (size_t)elem->datalen - 1 < min)
        return NULL;
    if (out_len)
        *out_len = (u8)(elem->datalen - 1);
    return elem->data + 1;
}

/* The 802.11ax elements, accepted on the conditions ieee802_11_parse_elems() sets. */
static const u8 *mlme_he_cap_ie(const u8 *ies, size_t len, u8 *cap_len)
{
    const u8 *cap = mlme_ext_elem(WLAN_EID_EXT_HE_CAPABILITY, ies, len,
                                  sizeof(struct ieee80211_he_cap_elem), cap_len);

    return cap && ieee80211_he_capa_size_ok(cap, *cap_len) ? cap : NULL;
}

static const struct ieee80211_he_operation *mlme_he_oper_ie(const u8 *ies, size_t len)
{
    u8 oper_len;
    const u8 *oper = mlme_ext_elem(WLAN_EID_EXT_HE_OPERATION, ies, len,
                                   sizeof(struct ieee80211_he_operation), &oper_len);

    /* ieee80211_he_oper_size() counts the extension id as well */
    return oper && oper_len >= ieee80211_he_oper_size(oper) - 1 ? (const void *)oper : NULL;
}

static const struct ieee80211_sta_he_cap *mlme_own_he_cap(void)
{
    return ieee80211_get_he_iftype_cap_vif(mlme.sband, mlme.vif);
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

/* What the AP's RSN element allows us to use. Returns 0 if CCMP with WPA2-PSK
 * (or, with @external, 802.1X) works. */
/*
 * What the AP's RSN element offers: the ciphers (CCMP for both pairwise and
 * group traffic, the only ones taken), the key management suites and the
 * capabilities. Fills mlme.group_cipher, ap_akms and ap_rsn_caps.
 */
static int mlme_parse_rsn(const struct element *rsn)
{
    static const u8 suite_ccmp[4] = { 0x00, 0x0f, 0xac, 4 };
    static const u8 suite_bip[4] = { 0x00, 0x0f, 0xac, 6 };
    const u8 *p = rsn->data, *end = rsn->data + rsn->datalen;
    bool ccmp = false;
    u16 count;

    mlme.ap_akms = 0;
    mlme.ap_rsn_caps = 0;
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
        if (p[0] == 0x00 && p[1] == 0x0f && p[2] == 0xac && p[3] < 32)
            mlme.ap_akms |= BIT(p[3]);

    if (end - p >= 2) {
        mlme.ap_rsn_caps = get_unaligned_le16(p);
        p += 2;
    }
    /* PMKIDs, then the group management cipher: BIP-CMAC-128 when absent */
    if (end - p >= 2) {
        count = get_unaligned_le16(p);
        p += 2;
        if (end - p < count * 16)
            return -EINVAL;
        p += count * 16;
    }
    if (end - p >= 4 && memcmp(p, suite_bip, 4) && (mlme.ap_rsn_caps & RSN_CAP_MFPC)) {
        if (mlme.ap_rsn_caps & RSN_CAP_MFPR) {
            mlme_info("the network protects management frames with %02x-%02x-%02x:%u, "
                      "not BIP-CMAC-128", p[0], p[1], p[2], p[3]);
            return -EOPNOTSUPP;
        }
        /* optional: not used */
        mlme.ap_rsn_caps &= ~RSN_CAP_MFPC;
    }

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
    return 0;
}

/*
 * The key management to use, from what the AP offers and what we have: SAE
 * when there is a password and the AP offers it (WPA3-Personal, also on a
 * WPA2/WPA3 network, as wpa_supplicant prefers it), else PSK. Management
 * frame protection when SAE is used or the AP requires it, as wpa_supplicant
 * does by default; and the RSN element for the association request, which
 * message 2 of the 4-way handshake repeats byte for byte.
 */
static int mlme_choose_akm(bool sae, bool psk)
{
    const u32 akms = mlme.ap_akms;
    const u16 caps = mlme.ap_rsn_caps;
    u8 *ie = mlme.own_rsn_ie;
    u16 own_caps = 0;

    if (sae && (akms & BIT(AKM_SAE)))
        mlme.akm = AKM_SAE;
    else if (psk && (akms & BIT(AKM_PSK)))
        mlme.akm = AKM_PSK;
    else if (psk && (akms & BIT(AKM_PSK_SHA256)))
        mlme.akm = AKM_PSK_SHA256;
    else if (akms & BIT(AKM_SAE)) {
        mlme_info("the network is WPA3-only, and no password came for it");
        return -EOPNOTSUPP;
    } else {
        mlme_info("the network offers no key management this driver does (0x%x)", akms);
        return -EOPNOTSUPP;
    }

    mlme.mfp = (caps & RSN_CAP_MFPC) &&
               (mlme.akm != AKM_PSK || (caps & RSN_CAP_MFPR));
    if (!mlme.mfp && (caps & RSN_CAP_MFPR)) {
        mlme_info("the network requires management frame protection it cannot negotiate");
        return -EOPNOTSUPP;
    }
    if (mlme.mfp)
        own_caps = RSN_CAP_MFPC | (mlme.akm == AKM_SAE ? RSN_CAP_MFPR : 0);
    mlme.key_ver = mlme.akm == AKM_PSK ? 2 : mlme.akm == AKM_PSK_SHA256 ? 3 : 0;

    ie[0] = WLAN_EID_RSN;
    ie[1] = 20;
    ie[2] = 1;                                          /* version */
    ie[3] = 0;
    memcpy(ie + 4, "\x00\x0f\xac\x04", 4);              /* group cipher: CCMP */
    memcpy(ie + 8, "\x01\x00\x00\x0f\xac\x04", 6);      /* pairwise: CCMP */
    memcpy(ie + 14, "\x01\x00\x00\x0f\xac", 5);          /* one AKM */
    ie[19] = mlme.akm;
    put_unaligned_le16(own_caps, ie + 20);
    mlme.own_rsn_len = 22;
    return 0;
}

/* The checks of an 802.1X network, whose RSN element macOS's supplicant writes. */
static int mlme_check_rsn_8021x(const struct element *rsn)
{
    int ret = mlme_parse_rsn(rsn);

    if (ret)
        return ret;
    if (!(mlme.ap_akms & BIT(AKM_8021X))) {
        mlme_info("the network does not offer 802.1X sign-in");
        return -EOPNOTSUPP;
    }
    if (mlme.ap_rsn_caps & RSN_CAP_MFPR) {
        mlme_info("the network requires management frame protection");
        return -EOPNOTSUPP;
    }
    mlme.akm = AKM_8021X;
    mlme.key_ver = 2;
    mlme.mfp = false;
    return 0;
}

static struct sk_buff *mlme_alloc_frame(size_t len)
{
    struct sk_buff *skb = dev_alloc_skb(mlme.hw->extra_tx_headroom + len);

    if (skb)
        skb_reserve(skb, mlme.hw->extra_tx_headroom);
    return skb;
}

/* Hand a frame of our own to the driver, as ieee80211_tx_skb() ends up doing. */
static void mlme_tx_frame(struct sk_buff *skb, u32 flags)
{
    struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
    struct ieee80211_tx_control control = { .sta = mlme.sta };

    memset(info, 0, sizeof(*info));
    info->flags = flags;
    info->control.vif = mlme.vif;
    info->band = mlme.chan->band;
    info->hw_queue = mlme.vif->hw_queue[IEEE80211_AC_VO];
    skb->priority = 7;
    skb_set_queue_mapping(skb, IEEE80211_AC_VO);

    if (mlme.tx_tap)
        mlme.tx_tap(skb->data, skb->len);
    mlme.local->ops->tx(mlme.hw, &control, skb);
}

/*
 * A robust management frame under 802.11w (deauthentication, disassociation,
 * most action frames), encrypted with the pairwise key. The hardware of most
 * of these chips does not encrypt management frames (hw_mgmt_tx_encrypt), and
 * mac80211 does it in software (IEEE80211_KEY_FLAG_SW_MGMT_TX); so does this
 * for all of them. The packet number is the data path's, one counter per key.
 */
static struct sk_buff *mlme_protect(struct sk_buff *skb)
{
    const size_t hdrlen = sizeof(struct ieee80211_hdr_3addr);
    struct ieee80211_key_conf *key = mlme.ptk_conf;
    size_t body_len = skb->len - hdrlen;
    struct sk_buff *out;
    u8 *hdr, *ccmp, *body;
    u64 pn;

    out = mlme_alloc_frame(skb->len + IEEE80211_CCMP_HDR_LEN + IEEE80211_CCMP_MIC_LEN);
    if (!out) {
        kfree_skb(skb);
        return NULL;
    }
    pn = (u64)atomic64_inc_return(&key->tx_pn);
    hdr = skb_put(out, hdrlen);
    memcpy(hdr, skb->data, hdrlen);
    hdr[1] |= IEEE80211_FCTL_PROTECTED >> 8;
    ccmp = skb_put_zero(out, IEEE80211_CCMP_HDR_LEN);
    ccmp[0] = (u8)pn;
    ccmp[1] = (u8)(pn >> 8);
    ccmp[3] = 0x20 | (u8)(key->keyidx << 6);    /* extended IV, key id */
    ccmp[4] = (u8)(pn >> 16);
    ccmp[5] = (u8)(pn >> 24);
    ccmp[6] = (u8)(pn >> 32);
    ccmp[7] = (u8)(pn >> 40);
    body = skb_put(out, body_len);
    memcpy(body, skb->data + hdrlen, body_len);
    rtw89_ccmp_encrypt(key->key, hdr, hdrlen, pn, body, body_len,
                       skb_put(out, IEEE80211_CCMP_MIC_LEN));
    kfree_skb(skb);
    return out;
}

/* Whether a management frame is robust (12.6.6.2), @len bytes long. */
static bool mlme_robust(const u8 *frame, size_t len)
{
    const struct ieee80211_mgmt *mgmt = (const void *)frame;

    if (ieee80211_is_action(mgmt->frame_control) && len < IEEE80211_MIN_ACTION_SIZE(category))
        return false;
    return _ieee80211_is_robust_mgmt_frame((struct ieee80211_hdr *)frame);
}

static void mlme_tx_mgmt(struct sk_buff *skb)
{
    if (mlme.mfp && mlme.ptk_conf && mlme_robust(skb->data, skb->len)) {
        skb = mlme_protect(skb);
        if (!skb)
            return;
    }
    mlme_tx_frame(skb, 0);
}

/*
 * A protected management frame from the AP: decrypted (by the hardware if it
 * did, else here), its packet number checked against replays and the CCMP
 * header and MIC removed, so it reads like any other. False if it is to be
 * dropped.
 */
static bool mlme_unprotect(struct sk_buff *skb)
{
    const size_t hdrlen = sizeof(struct ieee80211_hdr_3addr);
    const struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(skb);
    u8 *hdr = skb->data, *ccmp = hdr + hdrlen;
    size_t body_len;
    u64 pn;

    if (!mlme.mfp || !mlme.ptk_conf ||
        skb->len < hdrlen + IEEE80211_CCMP_HDR_LEN + IEEE80211_CCMP_MIC_LEN ||
        !(ccmp[3] & 0x20))
        return false;
    pn = ccmp[0] | (u64)ccmp[1] << 8 | (u64)ccmp[4] << 16 | (u64)ccmp[5] << 24 |
         (u64)ccmp[6] << 32 | (u64)ccmp[7] << 40;
    if (mlme.mgmt_rx_pn_valid && pn <= mlme.mgmt_rx_pn)
        return false;
    body_len = skb->len - hdrlen - IEEE80211_CCMP_HDR_LEN - IEEE80211_CCMP_MIC_LEN;
    if (!(status->flag & RX_FLAG_DECRYPTED) &&
        !rtw89_ccmp_decrypt(mlme.ptk_conf->key, hdr, hdrlen, pn,
                            ccmp + IEEE80211_CCMP_HDR_LEN, body_len,
                            ccmp + IEEE80211_CCMP_HDR_LEN + body_len))
        return false;
    mlme.mgmt_rx_pn = pn;
    mlme.mgmt_rx_pn_valid = true;

    memmove(hdr + IEEE80211_CCMP_HDR_LEN, hdr, hdrlen);
    skb_pull(skb, IEEE80211_CCMP_HDR_LEN);
    skb_trim(skb, skb->len - IEEE80211_CCMP_MIC_LEN);
    skb->data[1] &= ~(IEEE80211_FCTL_PROTECTED >> 8);
    return true;
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
    wiphy_delayed_work_cancel(mlme.hw->wiphy, &mlme.probe_timeout_work);
    wiphy_delayed_work_cancel(mlme.hw->wiphy, &mlme.bcn_mon_work);
    wiphy_delayed_work_cancel(mlme.hw->wiphy, &mlme.csa_work);
    mlme.poll = false;
    mlme.csa_pending = false;
    mlme.beacon_hash_valid = false;
    mlme.beacon_def_valid = false;

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
    rtw89_sae_end(mlme.sae);
    mlme.sae = NULL;
    mlme.sae_confirm_sent = false;
    mlme.sae_token_len = 0;
    wiphy_delayed_work_cancel(mlme.hw->wiphy, &mlme.sa_query_work);
    mlme.sa_query_pending = false;
    mlme.mgmt_rx_pn_valid = false;
    mlme.mfp = false;

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

    /* set while the association response was being taken in, which can fail
     * before the interface counts as associated */
    conf->he_support = false;
    conf->twt_requester = false;
    conf->twt_broadcast = false;
    conf->uora_exists = false;
    memset(&conf->he_bss_color, 0, sizeof(conf->he_bss_color));
    memset(&conf->he_oper, 0, sizeof(conf->he_oper));
    memset(&conf->he_obss_pd, 0, sizeof(conf->he_obss_pd));

    if (was_assoc) {
        rtw89_cfg80211_country_ie(mlme.hw->wiphy, NULL);
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
            if (ops->unassign_vif_chanctx)      /* as above */
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

/*
 * Authentication request: open system, or the SAE message due (our commit,
 * then our confirm once the AP's commit is in). The SAE commit itself was
 * computed when the join began; here it is only written out.
 */
static void mlme_send_auth(void)
{
    const size_t len = offsetof(struct ieee80211_mgmt, u.auth.variable);
    struct sk_buff *skb = mlme_alloc_frame(len + 512);
    struct ieee80211_mgmt *mgmt;
    int n = 0;

    if (!skb)
        return;
    mgmt = mlme_mgmt_header(skb, IEEE80211_STYPE_AUTH, len);
    mgmt->u.auth.auth_alg = cpu_to_le16(mlme.sae ? WLAN_AUTH_SAE : WLAN_AUTH_OPEN);
    mgmt->u.auth.auth_transaction = cpu_to_le16(mlme.sae && mlme.sae_confirm_sent ? 2 : 1);
    mgmt->u.auth.status_code = cpu_to_le16(0);
    if (mlme.sae) {
        u8 *body = skb_tail_pointer(skb);

        n = mlme.sae_confirm_sent ?
            rtw89_sae_write_confirm(mlme.sae, body, 512) :
            rtw89_sae_write_commit(mlme.sae, mlme.sae_token_len ? mlme.sae_token : NULL,
                                   mlme.sae_token_len, body, 512);
        if (n < 0) {
            kfree_skb(skb);
            mlme_fail(-ENOMEM, "could not write the SAE message");
            return;
        }
        skb_put(skb, n);
    }
    mlme_tx_mgmt(skb);

    mlme.tries++;
    wiphy_delayed_work_queue(mlme.hw->wiphy, &mlme.timeout_work,
                             mlme.sae ? MLME_SAE_TIMEOUT : MLME_AUTH_TIMEOUT);
}

/*
 * SAE did not get through on a network that takes WPA2-PSK as well (the AP
 * wants hash-to-element, another group, or never answers): join it with WPA2
 * instead, from the authentication on, as macOS would have before WPA3.
 * Returns false where there is nothing to fall back to.
 */
static bool mlme_sae_fall_back(const char *why)
{
    if (!mlme.sae || !mlme.sae_fallback || mlme_choose_akm(false, true))
        return false;
    mlme_info("%s: joining with WPA2 instead", why);
    rtw89_sae_end(mlme.sae);
    mlme.sae = NULL;
    mlme.sae_confirm_sent = false;
    mlme.sae_token_len = 0;
    mlme.have_pmk = true;
    mlme.tries = 0;
    wiphy_delayed_work_cancel(mlme.hw->wiphy, &mlme.timeout_work);
    mlme_send_auth();
    return true;
}

/* ------------------------------------------------------------------ */
/*  Association                                                         */
/* ------------------------------------------------------------------ */

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

/* ... and every one of them exists and has none of the flags @prohibited. */
static bool mlme_chandef_usable(const struct cfg80211_chan_def *def, u32 prohibited)
{
    unsigned int width = mlme_chandef_mhz(def), freq;
    unsigned int first = def->center_freq1 - width / 2 + 10;
    unsigned int last = def->center_freq1 + width / 2 - 10;
    struct ieee80211_channel *chan;

    if (!mlme_chandef_valid(def))
        return false;

    for (freq = first; freq <= last; freq += 20) {
        chan = ieee80211_get_channel(mlme.hw->wiphy, (int)freq);
        if (!chan || (chan->flags & prohibited))
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
 * The channel the AP operates on, from its HT, VHT and HE operation elements,
 * cut down to what this card and the regulatory domain allow. Also settles
 * which of 802.11n/ac/ax the join uses: mlme.ht, mlme.vht and mlme.he go in
 * as "both sides have the elements" and come out as what holds up.
 */
static void mlme_determine_chandef(struct cfg80211_chan_def *def)
{
    const struct ieee80211_sta_he_cap *own_he = mlme_own_he_cap();
    const struct ieee80211_ht_operation *ht_oper = NULL;
    const struct ieee80211_vht_operation *vht_oper;
    const struct ieee80211_he_operation *he_oper;
    struct ieee80211_vht_operation he_vht;
    const struct element *elem;
    struct cfg80211_chan_def wide;
    unsigned int max = 20;
    bool from_he;

    cfg80211_chandef_create(def, mlme.chan, mlme.ht ? NL80211_CHAN_HT20 : NL80211_CHAN_NO_HT);
    mlme.bw_limit = 20;

    elem = mlme_find_elem(WLAN_EID_HT_OPERATION, mlme.bss.ies, mlme.bss.ies_len);
    if (mlme.ht && elem && elem->datalen >= sizeof(*ht_oper))
        ht_oper = (const void *)elem->data;
    if (!ht_oper) {
        /* everything newer builds on 802.11n */
        cfg80211_chandef_create(def, mlme.chan, NL80211_CHAN_NO_HT);
        mlme.ht = false;
        mlme.vht = false;
        mlme.he = false;
        return;
    }

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

again:
    /* The wide channel: from the VHT operation element, or for an 802.11ax
     * AP from the copy of its fields in the HE operation element. */
    vht_oper = NULL;
    from_he = false;
    he_oper = mlme.he ? mlme_he_oper_ie(mlme.bss.ies, mlme.bss.ies_len) : NULL;
    elem = mlme_find_elem(WLAN_EID_VHT_OPERATION, mlme.bss.ies, mlme.bss.ies_len);
    if (mlme.vht && he_oper &&
        (le32_to_cpu(he_oper->he_oper_params) & IEEE80211_HE_OPERATION_VHT_OPER_INFO)) {
        memset(&he_vht, 0, sizeof(he_vht));
        memcpy(&he_vht, he_oper->optional, 3);
        vht_oper = &he_vht;
        from_he = true;
    } else if (mlme.vht && elem && elem->datalen >= sizeof(*vht_oper)) {
        vht_oper = (const void *)elem->data;
    } else {
        mlme.vht = false;
    }

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

            if (!mlme_chandef_valid(&wide) && from_he) {
                /* look again as an 802.11ac station */
                mlme_info("the AP's 802.11ax channel information is invalid: not using 802.11ax");
                mlme.he = false;
                goto again;
            }
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

    /* 802.11ax is 802.11ac and more on 5 GHz (an AP with HE but without VHT
     * elements there is joined as 802.11n); on 2.4 GHz it follows 802.11n. */
    if (mlme.chan->band != NL80211_BAND_2GHZ && !mlme.vht)
        mlme.he = false;

    /* ieee80211_determine_our_sta_mode(): the widths the 802.11ax side of the
     * card does */
    if (mlme.he) {
        u8 widths = own_he->he_cap_elem.phy_cap_info[0];

        if (!(widths & (mlme.chan->band == NL80211_BAND_2GHZ ?
                        IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_40MHZ_IN_2G :
                        IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_40MHZ_80MHZ_IN_5G)))
            max = 20;
    }
    mlme.bw_limit = max;

    while (mlme_chandef_mhz(def) > max)
        mlme_chandef_downgrade(def);
    /* narrower where the regulatory domain rules out part of the channel;
     * then that is also all we claim to be able to do */
    while (!mlme_chandef_usable(def, IEEE80211_CHAN_DISABLED) &&
           def->width != NL80211_CHAN_WIDTH_20 && def->width != NL80211_CHAN_WIDTH_20_NOHT) {
        mlme_chandef_downgrade(def);
        mlme.bw_limit = mlme_chandef_mhz(def);
    }
    if (mlme.he && !mlme_chandef_usable(def, IEEE80211_CHAN_NO_HE)) {
        mlme_info("802.11ax is not allowed on this channel");
        mlme.he = false;
    }
}

/*
 * ieee80211_verify_peer_he_mcs_support() and ieee80211_verify_sta_he_mcs_support():
 * the AP's 802.11ax rates are consistent, and we can do the ones it requires
 * of every station.
 */
static bool mlme_he_mcs_ok(const u8 *cap_ie, const struct ieee80211_he_operation *oper)
{
    const u8 *ap = cap_ie + sizeof(struct ieee80211_he_cap_elem);   /* rx, tx for 80 MHz */
    const u8 *own = (const u8 *)&mlme_own_he_cap()->he_mcs_nss_supp;
    u16 ap_rx = get_unaligned_le16(ap), ap_tx = get_unaligned_le16(ap + 2);
    u16 required = le16_to_cpu(oper->he_mcs_nss_set);
    int nss, i;

    /* every 802.11ax station does MCS 0-7 on one stream, both ways */
    if ((ap_tx & 3) == IEEE80211_HE_MCS_NOT_SUPPORTED ||
        (ap_rx & 3) == IEEE80211_HE_MCS_NOT_SUPPORTED)
        return false;

    /* all zero would mean eight streams; some APs (iPhones) send it anyway */
    if (!required)
        return true;

    for (nss = 0; nss < 8; nss++) {
        u8 need = (required >> 2 * nss) & 3;
        u8 rx = (ap_rx >> 2 * nss) & 3, tx = (ap_tx >> 2 * nss) & 3;

        if (need == IEEE80211_HE_MCS_NOT_SUPPORTED)
            continue;
        if (rx == IEEE80211_HE_MCS_NOT_SUPPORTED || tx == IEEE80211_HE_MCS_NOT_SUPPORTED ||
            rx < need || tx < need)
            return false;
    }

    /* our side: one of 80 MHz, 160 MHz and 80+80 MHz must cover the set */
    for (i = 0; i < 3; i++) {
        u16 own_rx = get_unaligned_le16(own + 4 * i), own_tx = get_unaligned_le16(own + 4 * i + 2);
        bool ok = true;

        for (nss = 0; nss < 8 && ok; nss++) {
            u8 need = (required >> 2 * nss) & 3;
            u8 rx = (own_rx >> 2 * nss) & 3, tx = (own_tx >> 2 * nss) & 3;

            if (need == IEEE80211_HE_MCS_NOT_SUPPORTED)
                continue;
            if (rx == IEEE80211_HE_MCS_NOT_SUPPORTED || tx == IEEE80211_HE_MCS_NOT_SUPPORTED ||
                need > rx || need > tx)
                ok = false;
        }
        if (ok)
            return true;
    }
    return false;
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

/* ieee80211_put_he_cap(): our 802.11ax capabilities, without what goes beyond
 * the widest channel we may use (ieee80211_get_adjusted_he_cap()). */
static void mlme_put_he_cap(struct sk_buff *skb)
{
    const struct ieee80211_sta_he_cap *own = mlme_own_he_cap();
    struct ieee80211_he_cap_elem elem = own->he_cap_elem;
    u8 ru_limit, max_ru, *len;

    ru_limit = mlme.bw_limit >= 160 ? IEEE80211_HE_PHY_CAP8_DCM_MAX_RU_2x996 :
               mlme.bw_limit >= 80 ? IEEE80211_HE_PHY_CAP8_DCM_MAX_RU_996 :
               mlme.bw_limit >= 40 ? IEEE80211_HE_PHY_CAP8_DCM_MAX_RU_484 :
               IEEE80211_HE_PHY_CAP8_DCM_MAX_RU_242;
    max_ru = elem.phy_cap_info[8] & IEEE80211_HE_PHY_CAP8_DCM_MAX_RU_MASK;
    elem.phy_cap_info[8] &= ~IEEE80211_HE_PHY_CAP8_DCM_MAX_RU_MASK;
    elem.phy_cap_info[8] |= min(max_ru, ru_limit);

    if (mlme.bw_limit < 40) {
        elem.phy_cap_info[0] &= ~(IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_40MHZ_80MHZ_IN_5G |
                                  IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_40MHZ_IN_2G);
        elem.phy_cap_info[9] &= ~IEEE80211_HE_PHY_CAP9_LONGER_THAN_16_SIGB_OFDM_SYM;
    }
    if (mlme.bw_limit < 160) {
        elem.phy_cap_info[0] &= ~(IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_160MHZ_IN_5G |
                                  IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_80PLUS80_MHZ_IN_5G);
        elem.phy_cap_info[5] &= ~IEEE80211_HE_PHY_CAP5_BEAMFORMEE_NUM_SND_DIM_ABOVE_80MHZ_MASK;
        elem.phy_cap_info[7] &= ~(IEEE80211_HE_PHY_CAP7_STBC_TX_ABOVE_80MHZ |
                                  IEEE80211_HE_PHY_CAP7_STBC_RX_ABOVE_80MHZ);
    }

    *(u8 *)skb_put(skb, 1) = WLAN_EID_EXTENSION;
    len = skb_put(skb, 1);
    *(u8 *)skb_put(skb, 1) = WLAN_EID_EXT_HE_CAPABILITY;
    skb_put_data(skb, &elem, sizeof(elem));
    /* the rate maps for 80 MHz, and for the wider channels still claimed */
    skb_put_data(skb, &own->he_mcs_nss_supp, ieee80211_he_mcs_nss_size(&elem));
    if (own->he_cap_elem.phy_cap_info[6] & IEEE80211_HE_PHY_CAP6_PPE_THRESHOLD_PRESENT)
        skb_put_data(skb, own->ppe_thres,
                     ieee80211_he_ppe_size(own->ppe_thres[0], own->he_cap_elem.phy_cap_info));
    *len = (u8)(skb_tail_pointer(skb) - len - 1);
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

    skb = mlme_alloc_frame(offsetof(struct ieee80211_mgmt, u.assoc_req.variable) + 512);
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
    if (mlme.rsn && mlme.external)
        skb_put_data(skb, mlme.ext_rsn_ie, mlme.ext_rsn_len);
    else if (mlme.rsn)
        skb_put_data(skb, mlme.own_rsn_ie, mlme.own_rsn_len);

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

    if (mlme.he)
        mlme_put_he_cap(skb);

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

/* ieee80211_he_mcs_intersection(): per stream, the rates one side sends and
 * the other receives. */
static void mlme_he_mcs_intersect(u16 own_rx, u16 own_tx, u16 *peer_rx_map, u16 *peer_tx_map)
{
    int i;

    for (i = 0; i < 8; i++) {
        u16 o_rx = (own_rx >> i * 2) & 3, o_tx = (own_tx >> i * 2) & 3;
        u16 p_rx = (*peer_rx_map >> i * 2) & 3, p_tx = (*peer_tx_map >> i * 2) & 3;

        if (p_tx != IEEE80211_HE_MCS_NOT_SUPPORTED) {
            if (o_rx == IEEE80211_HE_MCS_NOT_SUPPORTED)
                p_tx = IEEE80211_HE_MCS_NOT_SUPPORTED;
            else if (o_rx < p_tx)
                p_tx = o_rx;
        }
        if (p_rx != IEEE80211_HE_MCS_NOT_SUPPORTED) {
            if (o_tx == IEEE80211_HE_MCS_NOT_SUPPORTED)
                p_rx = IEEE80211_HE_MCS_NOT_SUPPORTED;
            else if (o_tx < p_rx)
                p_rx = o_tx;
        }
        *peer_rx_map = (u16)((*peer_rx_map & ~(3u << i * 2)) | (p_rx << i * 2));
        *peer_tx_map = (u16)((*peer_tx_map & ~(3u << i * 2)) | (p_tx << i * 2));
    }
}

#define MLME_HE_INTERSECT(own, peer, bw) do {                                       \
        u16 rx_ = le16_to_cpu((peer)->rx_mcs_##bw), tx_ = le16_to_cpu((peer)->tx_mcs_##bw); \
        mlme_he_mcs_intersect(le16_to_cpu((own)->rx_mcs_##bw),                      \
                              le16_to_cpu((own)->tx_mcs_##bw), &rx_, &tx_);         \
        (peer)->rx_mcs_##bw = cpu_to_le16(rx_);                                     \
        (peer)->tx_mcs_##bw = cpu_to_le16(tx_);                                     \
    } while (0)

/* ieee80211_he_cap_ie_to_sta_he_cap(): the same for 802.11ax. @ie is the
 * element's payload as mlme_he_cap_ie() returns it. */
static void mlme_set_sta_he_cap(const u8 *ie, u8 ie_len)
{
    struct ieee80211_link_sta *link_sta = &mlme.sta->deflink;
    const struct ieee80211_sta_he_cap *own = mlme_own_he_cap();
    struct ieee80211_sta_he_cap *he_cap = &link_sta->he_cap;
    const struct ieee80211_he_cap_elem *elem = (const void *)ie;
    struct ieee80211_he_mcs_nss_supp *peer = &he_cap->he_mcs_nss_supp;
    u8 mcs_nss_size, ppe_size = 0;
    bool own_wide, peer_wide;

    memset(he_cap, 0, sizeof(*he_cap));
    if (!ie || !own || !own->has_he)
        return;

    mcs_nss_size = ieee80211_he_mcs_nss_size(elem);
    if (ie_len < sizeof(*elem) + mcs_nss_size)
        return;
    if (elem->phy_cap_info[6] & IEEE80211_HE_PHY_CAP6_PPE_THRESHOLD_PRESENT) {
        if (ie_len < sizeof(*elem) + mcs_nss_size + 1)
            return;
        ppe_size = ieee80211_he_ppe_size(ie[sizeof(*elem) + mcs_nss_size], elem->phy_cap_info);
        if (ie_len < sizeof(*elem) + mcs_nss_size + ppe_size || ppe_size > sizeof(he_cap->ppe_thres))
            return;
    }

    memcpy(&he_cap->he_cap_elem, ie, sizeof(*elem));
    memcpy(peer, ie + sizeof(*elem), mcs_nss_size);
    memcpy(he_cap->ppe_thres, ie + sizeof(*elem) + mcs_nss_size, ppe_size);
    he_cap->has_he = true;

    MLME_HE_INTERSECT(&own->he_mcs_nss_supp, peer, 80);

    /* the wider channels: both sides, or not at all */
    own_wide = own->he_cap_elem.phy_cap_info[0] & IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_160MHZ_IN_5G;
    peer_wide = he_cap->he_cap_elem.phy_cap_info[0] &
                IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_160MHZ_IN_5G;
    if (peer_wide && own_wide) {
        MLME_HE_INTERSECT(&own->he_mcs_nss_supp, peer, 160);
    } else if (peer_wide) {
        peer->rx_mcs_160 = cpu_to_le16(0xffff);
        peer->tx_mcs_160 = cpu_to_le16(0xffff);
        he_cap->he_cap_elem.phy_cap_info[0] &= ~IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_160MHZ_IN_5G;
    }

    own_wide = own->he_cap_elem.phy_cap_info[0] &
               IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_80PLUS80_MHZ_IN_5G;
    peer_wide = he_cap->he_cap_elem.phy_cap_info[0] &
                IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_80PLUS80_MHZ_IN_5G;
    if (peer_wide && own_wide) {
        MLME_HE_INTERSECT(&own->he_mcs_nss_supp, peer, 80p80);
    } else if (peer_wide) {
        peer->rx_mcs_80p80 = cpu_to_le16(0xffff);
        peer->tx_mcs_80p80 = cpu_to_le16(0xffff);
        he_cap->he_cap_elem.phy_cap_info[0] &=
            ~IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_80PLUS80_MHZ_IN_5G;
    }
}

/* The highest stream with a rate in an 802.11ac/ax rate map (2 bits a stream). */
static u8 mlme_mcs_map_nss(u16 map)
{
    int i;

    for (i = 7; i >= 0; i--)
        if (((map >> i * 2) & 3) != IEEE80211_VHT_MCS_NOT_SUPPORTED)
            return (u8)(i + 1);
    return 0;
}

/* ieee80211_sta_init_nss_bw_capa(): how many streams and how wide a channel
 * we may use towards the AP. */
static void mlme_set_sta_nss_bw(void)
{
    struct ieee80211_link_sta *link_sta = &mlme.sta->deflink;
    const struct ieee80211_sta_he_cap *he_cap = &link_sta->he_cap;
    u8 ht_nss = 0, vht_nss = 0, he_nss = 0;
    unsigned int cap_mhz = 20, mhz;
    int i;

    /* ieee80211_sta_nss_capability() */
    for (i = 0; link_sta->ht_cap.ht_supported && i < 4; i++)
        if (link_sta->ht_cap.mcs.rx_mask[i])
            ht_nss++;
    if (link_sta->vht_cap.vht_supported)
        vht_nss = mlme_mcs_map_nss(le16_to_cpu(link_sta->vht_cap.vht_mcs.rx_mcs_map));
    if (he_cap->has_he) {
        he_nss = mlme_mcs_map_nss(le16_to_cpu(he_cap->he_mcs_nss_supp.rx_mcs_80));
        if (he_cap->he_cap_elem.phy_cap_info[0] & IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_160MHZ_IN_5G)
            he_nss = min(he_nss, mlme_mcs_map_nss(le16_to_cpu(he_cap->he_mcs_nss_supp.rx_mcs_160)));
    }
    link_sta->rx_nss = max_t(u8, 1, max(he_nss, max(vht_nss, ht_nss)));

    /* ieee80211_sta_bw_capability(), then no wider than the channel */
    if (he_cap->has_he) {
        u8 info = he_cap->he_cap_elem.phy_cap_info[0];

        if (mlme.sband->band == NL80211_BAND_2GHZ)
            cap_mhz = info & IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_40MHZ_IN_2G ? 40 : 20;
        else if (info & (IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_160MHZ_IN_5G |
                         IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_80PLUS80_MHZ_IN_5G))
            cap_mhz = 160;
        else if (info & IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_40MHZ_80MHZ_IN_5G)
            cap_mhz = 80;
    } else if (link_sta->vht_cap.vht_supported) {
        cap_mhz = 80;
    } else if (link_sta->ht_cap.cap & IEEE80211_HT_CAP_SUP_WIDTH_20_40) {
        cap_mhz = 40;
    }
    mhz = min(cap_mhz, mlme_chandef_mhz(&mlme.chandef));
    link_sta->bandwidth = mhz >= 80 ? IEEE80211_STA_RX_BW_80 :
                          mhz >= 40 ? IEEE80211_STA_RX_BW_40 : IEEE80211_STA_RX_BW_20;

    ieee80211_sta_recalc_aggregates(mlme.sta);
}

/*
 * The 802.11ax settings of the network, from the association response
 * (ieee80211_assoc_config_link()). Returns the BSS_CHANGED_* bits for them.
 */
static u64 mlme_set_bss_he(const struct ieee80211_he_operation *oper, const u8 *ies, size_t ies_len)
{
    struct ieee80211_bss_conf *conf = &mlme.vif->bss_conf;
    const struct ieee80211_sta_he_cap *he_cap = &mlme.sta->deflink.he_cap;
    const struct ieee80211_sta_he_cap *own = mlme_own_he_cap();
    const struct element *ext_capab = mlme_find_elem(WLAN_EID_EXT_CAPABILITY, ies, ies_len);
    const u8 *uora, *spr;
    u64 changed = 0;
    u32 params;
    u8 spr_len;

    conf->he_support = oper && he_cap->has_he;

    /* target wake times: only what both sides offer (this card: neither) */
    conf->twt_requester = conf->he_support && ext_capab && ext_capab->datalen >= 10 &&
        (ext_capab->data[9] & WLAN_EXT_CAPA10_TWT_RESPONDER_SUPPORT) &&
        (he_cap->he_cap_elem.mac_cap_info[0] & IEEE80211_HE_MAC_CAP0_TWT_RES) &&
        (own->he_cap_elem.mac_cap_info[0] & IEEE80211_HE_MAC_CAP0_TWT_REQ);
    conf->twt_protected = false;
    conf->twt_broadcast = conf->he_support &&
        (he_cap->he_cap_elem.mac_cap_info[2] & IEEE80211_HE_MAC_CAP2_BCAST_TWT) &&
        (own->he_cap_elem.mac_cap_info[2] & IEEE80211_HE_MAC_CAP2_BCAST_TWT);

    memset(&conf->he_bss_color, 0, sizeof(conf->he_bss_color));
    memset(&conf->he_oper, 0, sizeof(conf->he_oper));
    memset(&conf->he_obss_pd, 0, sizeof(conf->he_obss_pd));
    conf->uora_exists = false;
    if (!conf->he_support)
        return 0;

    params = le32_to_cpu(oper->he_oper_params);
    conf->he_bss_color.color = (u8)FIELD_GET(IEEE80211_HE_OPERATION_BSS_COLOR_MASK, params);
    conf->he_bss_color.partial = !!(params & IEEE80211_HE_OPERATION_PARTIAL_BSS_COLOR);
    conf->he_bss_color.enabled = !(params & IEEE80211_HE_OPERATION_BSS_COLOR_DISABLED);
    if (conf->he_bss_color.enabled)
        changed |= BSS_CHANGED_HE_BSS_COLOR;

    conf->htc_trig_based_pkt_ext = (u8)FIELD_GET(IEEE80211_HE_OPERATION_DFLT_PE_DURATION_MASK, params);
    conf->frame_time_rts_th = (u16)FIELD_GET(IEEE80211_HE_OPERATION_RTS_THRESHOLD_MASK, params);

    /* random access for uplink OFDMA */
    uora = mlme_ext_elem(WLAN_EID_EXT_UORA, ies, ies_len, 1, NULL);
    conf->uora_exists = uora != NULL;
    if (uora)
        conf->uora_ocw_range = uora[0];

    /* ieee80211_he_op_ie_to_bss_conf() */
    conf->he_oper.params = params;
    conf->he_oper.nss_set = le16_to_cpu(oper->he_mcs_nss_set);

    /* ieee80211_he_spr_ie_to_bss_conf(): spatial reuse */
    spr = mlme_ext_elem(WLAN_EID_EXT_HE_SPR, ies, ies_len, sizeof(struct ieee80211_he_spr), &spr_len);
    if (spr && spr_len >= ieee80211_he_spr_size(spr) - 1) {
        const u8 *data = spr + 1;

        conf->he_obss_pd.sr_ctrl = spr[0];
        if (spr[0] & IEEE80211_HE_SPR_NON_SRG_OFFSET_PRESENT)
            conf->he_obss_pd.non_srg_max_offset = *data++;
        if (spr[0] & IEEE80211_HE_SPR_SRG_INFORMATION_PRESENT) {
            conf->he_obss_pd.min_offset = *data++;
            conf->he_obss_pd.max_offset = *data++;
            memcpy(conf->he_obss_pd.bss_color_bitmap, data, 8);
            data += 8;
            memcpy(conf->he_obss_pd.partial_bssid_bitmap, data, 8);
            conf->he_obss_pd.enable = true;
        }
    }
    return changed;
}

/* EDCA parameters per access category (ieee80211_sta_wmm_params / set_wmm_default). */
static void mlme_conf_tx(const struct element *wmm, const struct ieee80211_mu_edca_param_set *mu_edca)
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
            /* 802.11ax: the parameters to contend with for a while after the
             * AP has scheduled our uplink itself */
            params[ac].mu_edca = mu_edca != NULL;
            if (mu_edca)
                params[ac].mu_edca_param_rec = aci == 1 ? mu_edca->ac_bk :
                                               aci == 2 ? mu_edca->ac_vi :
                                               aci == 3 ? mu_edca->ac_vo : mu_edca->ac_be;
        }
    }

    for (ac = 0; ac < IEEE80211_NUM_ACS; ac++)
        if (mlme.local->ops->conf_tx)
            mlme.local->ops->conf_tx(mlme.hw, mlme.vif, 0, ac, &params[ac]);

    /* which version of each set this was: beacons announce new ones by count */
    mlme.wmm_param_set = wmm && wmm->datalen >= 7 ? wmm->data[6] & 0x0f : -1;
    mlme.mu_edca_param_set = mu_edca ? mu_edca->mu_qos_info & 0x0f : -1;
}

/* ieee80211_handle_bss_capability(): protection, preamble and slot time from
 * the capability field and the ERP element. Returns what changed. */
static u64 mlme_bss_capability(u16 capab, const struct element *erp)
{
    struct ieee80211_bss_conf *conf = &mlme.vif->bss_conf;
    bool valid = erp && erp->datalen >= 1;
    bool cts_prot = valid && (erp->data[0] & WLAN_ERP_USE_PROTECTION);
    bool short_preamble = valid ? !(erp->data[0] & WLAN_ERP_BARKER_PREAMBLE) :
                                  !!(capab & WLAN_CAPABILITY_SHORT_PREAMBLE);
    bool short_slot = !!(capab & WLAN_CAPABILITY_SHORT_SLOT_TIME);
    u64 changed = 0;

    if (mlme.sband->band != NL80211_BAND_2GHZ) {
        short_slot = true;
        short_preamble = true;
    }
    if (conf->use_cts_prot != cts_prot) {
        conf->use_cts_prot = cts_prot;
        changed |= BSS_CHANGED_ERP_CTS_PROT;
    }
    if (conf->use_short_preamble != short_preamble) {
        conf->use_short_preamble = short_preamble;
        changed |= BSS_CHANGED_ERP_PREAMBLE;
    }
    if (conf->use_short_slot != short_slot) {
        conf->use_short_slot = short_slot;
        changed |= BSS_CHANGED_ERP_SLOT;
    }
    return changed;
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
    const struct ieee80211_he_operation *he_oper;
    const u8 *he_cap, *mu_edca = NULL;
    u16 capab, status, aid;
    u8 he_cap_len = 0;
    u64 changed;

    if (mlme.state != RTW89_MLME_ASSOCIATING || len < fixed)
        return;

    wiphy_delayed_work_cancel(mlme.hw->wiphy, &mlme.timeout_work);

    capab = le16_to_cpu(mgmt->u.assoc_resp.capab_info);
    status = le16_to_cpu(mgmt->u.assoc_resp.status_code);
    aid = le16_to_cpu(mgmt->u.assoc_resp.aid) & ~(BIT(15) | BIT(14));
    ies = mgmt->u.assoc_resp.variable;
    ies_len = len - fixed;

    if (status == WLAN_STATUS_ASSOC_REJECTED_TEMPORARILY && mlme.mfp &&
        mlme.tries < MLME_MAX_TRIES) {
        /*
         * 802.11w: the AP still has a protected association with us (we left
         * without it hearing) and first makes sure it is gone. The Timeout
         * Interval element says when to ask again (TUs).
         */
        unsigned long wait = HZ;

        elem = mlme_find_elem(WLAN_EID_TIMEOUT_INTERVAL, mgmt->u.assoc_resp.variable,
                              len - fixed);
        if (elem && elem->datalen >= 5 && elem->data[0] == WLAN_TIMEOUT_ASSOC_COMEBACK)
            wait = msecs_to_jiffies(get_unaligned_le32(elem->data + 1) * 1024 / 1000) + 1;
        if (wait > MLME_COMEBACK_MAX)
            wait = MLME_COMEBACK_MAX;
        mlme_info("the AP asks to come back in %u ms", jiffies_to_msecs(wait));
        wiphy_delayed_work_queue(mlme.hw->wiphy, &mlme.timeout_work, wait);
        return;
    }
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
    /* 802.11ax needs both elements in the response itself */
    he_oper = mlme.he ? mlme_he_oper_ie(ies, ies_len) : NULL;
    he_cap = he_oper ? mlme_he_cap_ie(ies, ies_len, &he_cap_len) : NULL;
    mlme_set_sta_he_cap(he_cap, he_cap_len);
    mlme_set_sta_nss_bw();

    mlme.sta->aid = aid;
    mlme.sta->mfp = mlme.mfp;
    mlme.sta->wme = wmm != NULL;
    mlme.sta->max_rx_aggregation_subframes = mlme.hw->max_rx_aggregation_subframes;
    mlme.aid = aid;

    /* the link: ERP, HT operation, QoS, DTIM */
    conf->assoc_capability = capab;
    mlme_bss_capability(capab, mlme_find_elem(WLAN_EID_ERP_INFO, mlme.bss.ies, mlme.bss.ies_len));
    elem = mlme.ht ? mlme_elem(WLAN_EID_HT_OPERATION, ies, ies_len) : NULL;
    conf->ht_operation_mode = elem && elem->datalen >= sizeof(struct ieee80211_ht_operation) ?
        le16_to_cpu(((const struct ieee80211_ht_operation *)elem->data)->operation_mode) : 0;
    conf->qos = wmm != NULL;
    elem = mlme_find_elem(WLAN_EID_TIM, mlme.bss.ies, mlme.bss.ies_len);
    conf->dtim_period = elem && elem->datalen >= 2 && elem->data[1] ? elem->data[1] : 1;
    changed = BSS_CHANGED_ERP_CTS_PROT | BSS_CHANGED_ERP_PREAMBLE | BSS_CHANGED_ERP_SLOT |
              BSS_CHANGED_HT | BSS_CHANGED_BASIC_RATES | BSS_CHANGED_QOS |
              BSS_CHANGED_BEACON_INFO;
    changed |= mlme_set_bss_he(he_cap ? he_oper : NULL, ies, ies_len);

    /* Linux takes the MU EDCA set from the response and then from every
     * beacon; beacons are not followed here, so the one from the scan stands in */
    if (conf->he_support) {
        mu_edca = mlme_ext_elem(WLAN_EID_EXT_HE_MU_EDCA, ies, ies_len,
                                sizeof(struct ieee80211_mu_edca_param_set), NULL);
        if (!mu_edca)
            mu_edca = mlme_ext_elem(WLAN_EID_EXT_HE_MU_EDCA, mlme.bss.ies, mlme.bss.ies_len,
                                    sizeof(struct ieee80211_mu_edca_param_set), NULL);
    }
    mlme_conf_tx(wmm, (const void *)mu_edca);

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

    /* __cfg80211_connect_result(): the AP's country, for the TX power tables */
    elem = mlme_find_elem(WLAN_EID_COUNTRY, mlme.bss.ies, mlme.bss.ies_len);
    if (elem && elem->datalen >= 2)
        rtw89_cfg80211_country_ie(mlme.hw->wiphy, elem->data);

    mlme.last_error = 0;
    if (!mlme.rsn)
        rtw89_data_authorize();
    /* With an outside supplicant the link is up from here, so that its EAPOL
     * frames can pass; data waits for the keys. How long the sign-in may take
     * (a certificate to accept, a password to type) is its business. */
    mlme_set_state(mlme.rsn && !mlme.external ? RTW89_MLME_ASSOCIATED : RTW89_MLME_CONNECTED);
    mlme_bcn_mon_start();
    /* an AP that never starts or finishes the handshake must not leave us
     * associated without keys for ever */
    if (mlme.rsn && !mlme.external)
        wiphy_delayed_work_queue(mlme.hw->wiphy, &mlme.timeout_work, MLME_KEY_TIMEOUT);
    mlme_info("associated with %02x:%02x:%02x:%02x:%02x:%02x on %u MHz, AID %u, %s%s%s",
              mlme.bss.bssid[0], mlme.bss.bssid[1], mlme.bss.bssid[2],
              mlme.bss.bssid[3], mlme.bss.bssid[4], mlme.bss.bssid[5],
              mlme.bss.freq, aid,
              mlme.sta->deflink.he_cap.has_he ? "802.11ax" :
              mlme.sta->deflink.vht_cap.vht_supported ? "802.11ac" :
              mlme.sta->deflink.ht_cap.ht_supported ? "802.11n" : "802.11a/b/g",
              wmm ? ", WMM" : "", !mlme.rsn ? "" : mlme.external ?
              ", sign-in and keys left to the system" : ", waiting for the key handshake");
}

/* Authenticated (open system, or SAE with both confirms good): associate. */
static void mlme_authenticated(void)
{
    /* ieee80211_mark_sta_auth() */
    if (mlme_sta_move(IEEE80211_STA_AUTH)) {
        mlme_fail(-EIO, "authentication failed in the driver");
        return;
    }

    mlme.tries = 0;
    mlme_set_state(RTW89_MLME_ASSOCIATING);
    mlme_send_assoc();
}

/*
 * The AP's SAE messages (12.4.8.6): its commit, answered with our confirm,
 * and its confirm, which proves it has the same password. A request for an
 * anti-clogging token is answered with the commit again, the token in it.
 */
static void mlme_rx_auth_sae(u16 transaction, u16 status, const u8 *body, size_t len)
{
    u8 pmkid[16];
    int ret;

    if (transaction == 1) {
        if (status == WLAN_STATUS_ANTI_CLOG_REQUIRED && !mlme.sae_confirm_sent) {
            /* the group, then the token */
            if (len < 2 || len - 2 > sizeof(mlme.sae_token))
                return;
            wiphy_delayed_work_cancel(mlme.hw->wiphy, &mlme.timeout_work);
            memcpy(mlme.sae_token, body + 2, len - 2);
            mlme.sae_token_len = (u8)(len - 2);
            mlme.tries = 0;
            mlme_send_auth();
            return;
        }
        if (status != WLAN_STATUS_SUCCESS) {
            char why[64];

            snprintf(why, sizeof(why), "the AP refused the SAE commit, status %u%s", status,
                     status == WLAN_STATUS_SAE_HASH_TO_ELEMENT ? " (hash-to-element only)" : "");
            if (!mlme_sae_fall_back(why))
                mlme_fail(-1000 - status, why);
            return;
        }
        if (mlme.sae_confirm_sent) {
            /* its commit again: our confirm was lost; the timer resends it */
            return;
        }
        wiphy_delayed_work_cancel(mlme.hw->wiphy, &mlme.timeout_work);
        ret = rtw89_sae_rx_commit(mlme.sae, body, len);
        if (ret) {
            if (!mlme_sae_fall_back("the AP's SAE commit does not check out"))
                mlme_fail(-1000 - ret, "the AP's SAE commit does not check out");
            return;
        }
        mlme.sae_confirm_sent = true;
        mlme.tries = 0;
        mlme_send_auth();
        return;
    }

    if (transaction != 2 || !mlme.sae_confirm_sent)
        return;
    wiphy_delayed_work_cancel(mlme.hw->wiphy, &mlme.timeout_work);
    if (status != WLAN_STATUS_SUCCESS) {
        mlme_fail(-1000 - status, "the AP refused our SAE confirm");
        return;
    }
    if (rtw89_sae_rx_confirm(mlme.sae, body, len)) {
        /* the AP has a different password: it does not answer ours either */
        mlme_fail(-2000 - WLAN_REASON_PREV_AUTH_NOT_VALID,
                  "the AP's SAE confirm does not match: wrong password");
        return;
    }
    rtw89_sae_keys(mlme.sae, mlme.pmk, pmkid);
    memset(pmkid, 0, sizeof(pmkid));
    mlme.have_pmk = true;
    rtw89_sae_end(mlme.sae);
    mlme.sae = NULL;
    mlme.sae_confirm_sent = false;
    mlme_info("SAE done: the AP has the same password");
    mlme_authenticated();
}

static void mlme_rx_auth(const struct ieee80211_mgmt *mgmt, size_t len)
{
    const size_t fixed = offsetof(struct ieee80211_mgmt, u.auth.variable);
    u16 alg, transaction, status;

    if (mlme.state != RTW89_MLME_AUTHENTICATING || len < fixed)
        return;

    alg = le16_to_cpu(mgmt->u.auth.auth_alg);
    transaction = le16_to_cpu(mgmt->u.auth.auth_transaction);
    status = le16_to_cpu(mgmt->u.auth.status_code);
    if (mlme.sae) {
        if (alg == WLAN_AUTH_SAE)
            mlme_rx_auth_sae(transaction, status, mgmt->u.auth.variable, len - fixed);
        return;
    }
    if (alg != WLAN_AUTH_OPEN || transaction != 2)
        return;

    wiphy_delayed_work_cancel(mlme.hw->wiphy, &mlme.timeout_work);

    if (status != WLAN_STATUS_SUCCESS) {
        mlme_fail(-1000 - status, "the AP refused authentication");
        return;
    }
    mlme_authenticated();
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
#define KEY_INFO_VER_SHA1_AES   2       /* HMAC-SHA1 MIC (PSK, 802.1X) */
#define KEY_INFO_VER_CMAC_AES   3       /* AES-CMAC MIC (PSK-SHA256) */
#define KEY_INFO_VER_AKM        0       /* as the AKM says: SAE, AES-CMAC */
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

/* The MIC of an EAPOL-Key frame (with its MIC field zero) under @kck: HMAC-SHA1
 * for descriptor version 2, AES-CMAC for PSK-SHA256 and SAE. */
static void mlme_key_mic(const u8 kck[16], const u8 *frame, size_t len, u8 mic[16])
{
    u8 full[RTW89_SHA1_LEN];

    if (mlme.key_ver == KEY_INFO_VER_SHA1_AES) {
        rtw89_hmac_sha1(kck, 16, frame, len, full);
        memcpy(mic, full, 16);
        memset(full, 0, sizeof(full));
    } else {
        rtw89_aes_cmac(kck, frame, len, mic);
    }
}

/* Build and send an EAPOL-Key reply with a valid MIC. */
static void mlme_send_eapol_key(const struct mlme_ptk *ptk, u8 version, u16 key_info,
                                const u8 *replay, const u8 *nonce,
                                const u8 *key_data, size_t key_data_len)
{
    u8 frame[EAPOL_HDR_LEN + EAPOL_KEY_FIXED_LEN + sizeof(mlme.ext_rsn_ie)];
    u8 *k = frame + EAPOL_HDR_LEN;
    size_t len = EAPOL_HDR_LEN + EAPOL_KEY_FIXED_LEN + key_data_len;

    if (key_data_len > sizeof(mlme.ext_rsn_ie))
        return;

    memset(frame, 0, sizeof(frame));
    frame[0] = version;
    frame[1] = EAPOL_TYPE_KEY;
    put_unaligned_be16((u16)(EAPOL_KEY_FIXED_LEN + key_data_len), frame + 2);
    k[0] = EAPOL_KEY_DESC_RSN;
    put_unaligned_be16(key_info | mlme.key_ver | KEY_INFO_MIC, k + KEY_OFF_INFO);
    memcpy(k + KEY_OFF_REPLAY, replay, 8);
    if (nonce)
        memcpy(k + KEY_OFF_NONCE, nonce, 32);
    put_unaligned_be16((u16)key_data_len, k + KEY_OFF_DATA_LEN);
    if (key_data_len)
        memcpy(k + EAPOL_KEY_FIXED_LEN, key_data, key_data_len);

    /* the MIC covers the whole frame with the MIC field zero */
    mlme_key_mic(ptk->kck, frame, len, k + KEY_OFF_MIC);

    mlme_tx_eapol(frame, len);
}

/* PTK = PRF-384(PMK, "Pairwise key expansion",
 *               min(AA, SPA) || max(AA, SPA) || min(ANonce, SNonce) || max(...)),
 * the PRF being KDF-SHA-256 for PSK-SHA256 and SAE (12.7.1.3) */
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

    if (mlme.key_ver == KEY_INFO_VER_SHA1_AES)
        rtw89_sha1_prf(mlme.pmk, sizeof(mlme.pmk), "Pairwise key expansion",
                       data, sizeof(data), out, sizeof(out));
    else
        rtw89_kdf_sha256(mlme.pmk, sizeof(mlme.pmk), "Pairwise key expansion",
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
    u8 mic[16];

    if (mlme.tptk_valid) {
        mlme_key_mic(mlme.tptk.kck, copy, len, mic);
        if (rtw89_crypto_equal(mic, their_mic, 16)) {
            mlme.ptk = mlme.tptk;
            mlme.ptk_valid = true;
            memset(&mlme.tptk, 0, sizeof(mlme.tptk));
            mlme.tptk_valid = false;
            return true;
        }
    }
    if (mlme.ptk_valid) {
        mlme_key_mic(mlme.ptk.kck, copy, len, mic);
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

    if (!mlme.sta || mlme.sta_state < IEEE80211_STA_ASSOC)
        return;
    if (!mlme.have_pmk) {
        /* 802.1X: the AP starts the handshake as soon as the sign-in is
         * through, the PMK reaches us a moment later */
        if (mlme.external && !(key_info & KEY_INFO_MIC) && len <= sizeof(mlme.held_m1)) {
            memcpy(mlme.held_m1, e, len);
            mlme.held_m1_len = (u16)len;
        }
        return;
    }
    if ((key_info & KEY_INFO_VERSION) != mlme.key_ver) {
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
        /* with the RSN element of the association request, byte for byte */
        if (mlme.external)
            mlme_send_eapol_key(&mlme.tptk, e[0], KEY_INFO_PAIRWISE, k + KEY_OFF_REPLAY,
                                mlme.snonce, mlme.ext_rsn_ie, mlme.ext_rsn_len);
        else
            mlme_send_eapol_key(&mlme.tptk, e[0], KEY_INFO_PAIRWISE, k + KEY_OFF_REPLAY,
                                mlme.snonce, mlme.own_rsn_ie, mlme.own_rsn_len);
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
            mlme_info("%s handshake complete: keys installed, connected%s",
                      mlme.akm == AKM_SAE ? "WPA3" : "WPA2",
                      mlme.mfp ? ", management frames protected" : "");
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

/* ieee80211_add_addbaext(): the extension element 802.11ax stations add to
 * their ADDBA frames. @req_data: the element of the request being answered,
 * or 0. No fragments inside aggregates; the high bits of a buffer size above
 * 1023 are not for us. */
static void mlme_put_addba_ext(struct sk_buff *skb, u8 req_data, u16 buf_size)
{
    u8 *pos = skb_put_zero(skb, 2 + sizeof(struct ieee80211_addba_ext_ie));
    u8 data = IEEE80211_ADDBA_EXT_NO_FRAG;

    if (req_data)
        data &= req_data;
    data |= u8_encode_bits((u8)(buf_size >> IEEE80211_ADDBA_EXT_BUF_SIZE_SHIFT),
                           IEEE80211_ADDBA_EXT_BUF_SIZE_MASK);
    *pos++ = WLAN_EID_ADDBA_EXT;
    *pos++ = sizeof(struct ieee80211_addba_ext_ie);
    *pos = data;
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
    const size_t req_len = IEEE80211_MIN_ACTION_SIZE(addba_req);
    bool he = mlme.sta->deflink.he_cap.has_he;
    u16 max_buf_size = he ? IEEE80211_MAX_AMPDU_BUF_HE : IEEE80211_MAX_AMPDU_BUF_HT;
    u16 capab, tid, buf_size, ssn, status = WLAN_STATUS_SUCCESS;
    const struct element *ext;
    struct ieee80211_mgmt *mgmt;
    struct sk_buff *skb;
    u8 ext_data = 0;
    bool amsdu;

    if (len < req_len)
        return;
    /* ieee80211_retrieve_addba_ext_data() */
    ext = he ? mlme_find_elem(WLAN_EID_ADDBA_EXT, (const u8 *)req + req_len, len - req_len) : NULL;
    if (ext && ext->datalen >= sizeof(struct ieee80211_addba_ext_ie))
        ext_data = ext->data[0];
    capab = le16_to_cpu(req->u.action.addba_req.capab);
    tid = (capab & IEEE80211_ADDBA_PARAM_TID_MASK) >> 2;
    buf_size = (capab & IEEE80211_ADDBA_PARAM_BUF_SIZE_MASK) >> 6;
    ssn = le16_to_cpu(req->u.action.addba_req.start_seq_num) >> 4;
    amsdu = (capab & IEEE80211_ADDBA_PARAM_AMSDU_MASK) &&
            ieee80211_hw_check(mlme.hw, SUPPORTS_AMSDU_IN_AMPDU);

    /* zero means "as many as you can take" */
    if (!buf_size || buf_size > max_buf_size)
        buf_size = max_buf_size;
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

    skb = mlme_alloc_frame(resp_len + 2 + sizeof(struct ieee80211_addba_ext_ie));
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
    if (he)
        mlme_put_addba_ext(skb, ext_data, buf_size);
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
    bool he = mlme.sta && mlme.sta->deflink.he_cap.has_he;
    struct mlme_tx_ba *ba = &mlme.tx_ba[tid];
    struct ieee80211_mgmt *mgmt;
    struct sk_buff *skb;
    u16 buf_size;

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

    /* Towards an 802.11ax AP, what the card can send (up to 256). Otherwise
     * the HT maximum whatever the driver will really send: mac80211 does the
     * same because some APs mishandle smaller ones. */
    buf_size = he ? min_t(u16, mlme.hw->max_tx_aggregation_subframes, IEEE80211_MAX_AMPDU_BUF_HE) :
                    IEEE80211_MAX_AMPDU_BUF_HT;

    skb = mlme_alloc_frame(len + 2 + sizeof(struct ieee80211_addba_ext_ie));
    if (skb) {
        mgmt = mlme_mgmt_header(skb, IEEE80211_STYPE_ACTION, len);
        mgmt->u.action.category = WLAN_CATEGORY_BACK;
        mgmt->u.action.action_code = WLAN_ACTION_ADDBA_REQ;
        mgmt->u.action.addba_req.dialog_token = ba->dialog;
        mgmt->u.action.addba_req.capab =
            cpu_to_le16(IEEE80211_ADDBA_PARAM_AMSDU_MASK | IEEE80211_ADDBA_PARAM_POLICY_MASK |
                        (u16)(tid << 2) | (u16)(buf_size << 6));
        mgmt->u.action.addba_req.timeout = 0;
        mgmt->u.action.addba_req.start_seq_num = cpu_to_le16((u16)(ba->ssn << 4));
        if (he)
            mlme_put_addba_ext(skb, 0, buf_size);
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

/* ------------------------------------------------------------------ */
/*  SA Query (802.11w, 11.13; net/mac80211/mlme.c and wpa_supplicant)  */
/* ------------------------------------------------------------------ */

static void mlme_send_sa_query(u8 action, const u8 *id)
{
    const size_t len = IEEE80211_MIN_ACTION_SIZE(sa_query);
    struct sk_buff *skb = mlme_alloc_frame(len);
    struct ieee80211_mgmt *mgmt;

    if (!skb)
        return;
    mgmt = mlme_mgmt_header(skb, IEEE80211_STYPE_ACTION, len);
    mgmt->u.action.category = WLAN_CATEGORY_SA_QUERY;
    mgmt->u.action.action_code = action;
    memcpy(mgmt->u.action.sa_query.trans_id, id, WLAN_SA_QUERY_TR_ID_LEN);
    mlme_tx_mgmt(skb);
}

/* The AP asks whether we are still associated (it is about to drop us if no
 * protected answer comes), or answers our question. */
static void mlme_rx_sa_query(const struct ieee80211_mgmt *mgmt, size_t len)
{
    if (!mlme.mfp || len < IEEE80211_MIN_ACTION_SIZE(sa_query))
        return;
    if (mgmt->u.action.action_code == WLAN_ACTION_SA_QUERY_REQUEST) {
        mlme_send_sa_query(WLAN_ACTION_SA_QUERY_RESPONSE, mgmt->u.action.sa_query.trans_id);
    } else if (mgmt->u.action.action_code == WLAN_ACTION_SA_QUERY_RESPONSE &&
               mlme.sa_query_pending &&
               !memcmp(mgmt->u.action.sa_query.trans_id, mlme.sa_query_id,
                       WLAN_SA_QUERY_TR_ID_LEN)) {
        mlme.sa_query_pending = false;
        wiphy_delayed_work_cancel(mlme.hw->wiphy, &mlme.sa_query_work);
        mlme_info("the AP still knows us: an unprotected leave message was ignored");
    }
}

/*
 * An unprotected deauthentication or disassociation while management frames
 * are protected: anyone could have sent it. Ask the AP, protected, whether
 * it still has us; if it does not answer, it really dropped us (it lost the
 * keys, say after a restart).
 */
static void mlme_sa_query_start(u16 reason)
{
    if (mlme.sa_query_pending)
        return;
    get_random_bytes(mlme.sa_query_id, sizeof(mlme.sa_query_id));
    mlme.sa_query_pending = true;
    mlme.sa_query_reason = reason;
    mlme_send_sa_query(WLAN_ACTION_SA_QUERY_REQUEST, mlme.sa_query_id);
    wiphy_delayed_work_queue(mlme.hw->wiphy, &mlme.sa_query_work, MLME_SA_QUERY_WAIT);
}

static void mlme_sa_query_work(struct wiphy *wiphy, struct wiphy_work *work)
{
    if (!mlme.sa_query_pending || mlme.state == RTW89_MLME_IDLE)
        return;
    mlme.sa_query_pending = false;
    mlme.last_error = -2000 - mlme.sa_query_reason;
    mlme_info("no answer to the SA Query: the AP did drop us (reason %u)",
              mlme.sa_query_reason);
    mlme_teardown(0);
}

static void mlme_rx_action(const struct ieee80211_mgmt *mgmt, size_t len)
{
    if (mlme.state < RTW89_MLME_ASSOCIATED || len < IEEE80211_MIN_ACTION_SIZE(action_code))
        return;
    if (mgmt->u.action.category == WLAN_CATEGORY_SA_QUERY) {
        mlme_rx_sa_query(mgmt, len);
        return;
    }
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

/* ------------------------------------------------------------------ */
/*  While connected: is the AP still there, and what its beacons say    */
/* ------------------------------------------------------------------ */

/* ieee80211_reset_ap_probe() */
static void mlme_reset_ap_probe(void)
{
    if (!mlme.poll)
        return;
    mlme.poll = false;
    wiphy_delayed_work_cancel(mlme.hw->wiphy, &mlme.probe_timeout_work);
}

/*
 * ieee80211_mgd_probe_ap_send(): a null data frame the AP has to
 * acknowledge. The driver reports whether it did (REPORTS_TX_ACK_STATUS),
 * and rtw89_mlme_tx_status() takes the answer.
 */
static void mlme_probe_send(void)
{
    struct sk_buff *skb;

    mlme.probe_tries++;
    mlme.probe_failed = false;
    skb = ieee80211_nullfunc_get(mlme.hw, mlme.vif, -1,
                                 !ieee80211_hw_check(mlme.hw, DOESNT_SUPPORT_QOS_NDP));
    if (skb)
        mlme_tx_frame(skb, IEEE80211_TX_INTFL_DONT_ENCRYPT | IEEE80211_TX_CTL_REQ_TX_STATUS |
                           IEEE80211_TX_CTL_USE_MINRATE);

    mlme.probe_timeout = jiffies + MLME_PROBE_WAIT;
    wiphy_delayed_work_queue(mlme.hw->wiphy, &mlme.probe_timeout_work, MLME_PROBE_WAIT);
}

/* The poll part of ieee80211_sta_work(): after a status report or a timeout. */
static void mlme_probe_check(void)
{
    if (!mlme.running || !mlme.poll || mlme.state < RTW89_MLME_ASSOCIATED)
        return;

    if (!mlme.probe_tries) {
        mlme_info("the access point acknowledged a probe: still connected");
        mlme.probe_acks++;
        mlme_reset_ap_probe();
    } else if (mlme.probe_failed && mlme.probe_tries < MLME_PROBE_TRIES) {
        mlme_probe_send();
    } else if (!mlme.probe_failed && time_before(jiffies, mlme.probe_timeout)) {
        wiphy_delayed_work_queue(mlme.hw->wiphy, &mlme.probe_timeout_work,
                                 mlme.probe_timeout - jiffies);
    } else {
        mlme_info("the access point does not answer (%s): disconnecting",
                  mlme.probe_failed ? "probe not acknowledged" :
                                      "the driver did not report what became of the probe");
        mlme.last_error = -ENOLINK;
        /* no farewell: there is nobody to hear it */
        mlme_teardown(0);
    }
}

static void mlme_probe_work(struct wiphy *wiphy, struct wiphy_work *work)
{
    mlme_probe_check();
}

/* ieee80211_mgd_probe_ap(): the driver has missed beacons. */
static void mlme_beacon_loss_work(struct wiphy *wiphy, struct wiphy_work *work)
{
    if (!mlme.running || mlme.state < RTW89_MLME_ASSOCIATED)
        return;
    mlme.beacon_losses++;
    /* the driver keeps reporting while the AP is silent: one round at a time */
    if (mlme.poll)
        return;
    mlme_info("beacons from the access point are missing: probing it");
    mlme.poll = true;
    mlme.probe_tries = 0;
    mlme_probe_send();
}

/*
 * ieee80211_sta_conn_mon_timer(): when the driver does not watch the beacons
 * itself (no CONNECTION_MONITOR: rtw89 sets it only when the firmware filters
 * beacons, which the RTL8851B's and RTL8852A's do not), mac80211 does. Seven
 * beacon intervals without one, and never less than MLME_BEACON_LOSS_MIN (a
 * connected scan takes the radio away for about a second): probe the AP as for
 * a reported loss, which ends the connection if it does not answer.
 */
#define MLME_BEACON_LOSS_COUNT 7
#define MLME_BEACON_LOSS_MIN (3 * HZ)

static void mlme_bcn_mon_work(struct wiphy *wiphy, struct wiphy_work *work)
{
    unsigned int tu = mlme.bss.beacon_int ? mlme.bss.beacon_int : 100;
    unsigned long limit = max_t(unsigned long, MLME_BEACON_LOSS_MIN,
                                msecs_to_jiffies(MLME_BEACON_LOSS_COUNT * tu * 1024 / 1000));

    if (!mlme.running || mlme.state < RTW89_MLME_ASSOCIATED)
        return;
    if (!mlme.poll && time_after(jiffies, mlme.last_beacon + limit)) {
        mlme.last_beacon = jiffies;     /* one round of probing at a time */
        mlme_beacon_loss_work(wiphy, NULL);
    }
    wiphy_delayed_work_queue(wiphy, &mlme.bcn_mon_work, HZ);
}

static void mlme_bcn_mon_start(void)
{
    if (ieee80211_hw_check(mlme.hw, CONNECTION_MONITOR))
        return;
    mlme.last_beacon = jiffies;
    wiphy_delayed_work_queue(mlme.hw->wiphy, &mlme.bcn_mon_work, HZ);
}

/* Any context. */
void rtw89_mlme_beacon_loss(void)
{
    if (mlme.running)
        wiphy_work_queue(mlme.hw->wiphy, &mlme.beacon_loss_work);
}

/* ieee80211_sta_tx_notify(): the driver is done with a frame of ours. Any
 * context; the skb stays the caller's. */
void rtw89_mlme_tx_status(const struct sk_buff *skb)
{
    const struct ieee80211_hdr *hdr = (const void *)skb->data;
    const struct ieee80211_tx_info *info = IEEE80211_SKB_CB((struct sk_buff *)skb);

    if (mlme.running && skb->len >= sizeof(struct ieee80211_hdr_3addr) &&
        ieee80211_is_data(hdr->frame_control) &&
        (info->flags & IEEE80211_TX_CTL_REQ_TX_STATUS)) {
        size_t off = ieee80211_hdrlen(hdr->frame_control);

        /* our reply in the key handshake: acknowledged by the AP and still
         * no next message means it did not accept it (a wrong password); not
         * acknowledged means it did not get there */
        if (skb->len >= off + sizeof(mlme_eapol_llc) &&
            !memcmp(skb->data + off, mlme_eapol_llc, sizeof(mlme_eapol_llc))) {
            bool acked = info->flags & IEEE80211_TX_STAT_ACK;

            if (mlme.eapol_reports < 8) {
                mlme.eapol_reports++;
                mlme_info("key handshake reply %s by the AP",
                          acked ? "acknowledged" : "NOT acknowledged");
            }
            return;
        }
    }
    if (!mlme.running || !mlme.poll || skb->len < sizeof(struct ieee80211_hdr_3addr) ||
        !ieee80211_is_any_nullfunc(hdr->frame_control) ||
        !(info->flags & IEEE80211_TX_CTL_REQ_TX_STATUS))
        return;

    if (info->flags & IEEE80211_TX_STAT_ACK)
        mlme.probe_tries = 0;
    else
        mlme.probe_failed = true;
    wiphy_work_queue(mlme.hw->wiphy, &mlme.probe_work);
}

/* The AP has moved (or is about to): the connection ends here, and whoever
 * asked for it joins again on the new channel. */
static void mlme_csa_work(struct wiphy *wiphy, struct wiphy_work *work)
{
    if (!mlme.running || !mlme.csa_pending || mlme.state < RTW89_MLME_ASSOCIATED)
        return;
    mlme_info("the access point has changed channel: leaving to join it there");
    mlme.last_error = -ENETRESET;
    mlme_teardown(0);
}

/*
 * A channel switch announcement in a beacon. Linux follows the AP to the new
 * channel (ieee80211_sta_process_chanswitch()); this only waits until the
 * switch is due and then drops the connection.
 */
static void mlme_beacon_chanswitch(const u8 *ies, size_t len)
{
    const struct element *csa = mlme_find_elem(WLAN_EID_CHANNEL_SWITCH, ies, len);
    const struct element *ecsa = mlme_find_elem(WLAN_EID_EXT_CHANSWITCH_ANN, ies, len);
    unsigned int chan, count, tu;

    if (ecsa && ecsa->datalen >= 4) {           /* mode, operating class, channel, count */
        chan = ecsa->data[2];
        count = ecsa->data[3];
    } else if (csa && csa->datalen >= 3) {      /* mode, channel, count */
        chan = csa->data[1];
        count = csa->data[2];
    } else {
        return;
    }
    if (mlme.csa_pending)
        return;

    mlme.csa_pending = true;
    mlme_info("the access point announces a move to channel %u in %u beacon(s)", chan, count);
    /* the beacons left on this channel, and a little for the AP to settle */
    tu = count * (mlme.bss.beacon_int ? mlme.bss.beacon_int : 100) + 100;
    wiphy_delayed_work_queue(mlme.hw->wiphy, &mlme.csa_work,
                             usecs_to_jiffies(tu * 1024));
}

/* The channel the elements of a beacon describe, by the rules of the join. */
static void mlme_beacon_chandef(const u8 *ies, size_t len, struct cfg80211_chan_def *def)
{
    const u8 *saved_ies = mlme.bss.ies;
    size_t saved_len = mlme.bss.ies_len;
    unsigned int bw_limit = mlme.bw_limit;
    bool ht = mlme.ht, vht = mlme.vht, he = mlme.he;

    mlme.bss.ies = ies;
    mlme.bss.ies_len = len;
    mlme_determine_chandef(def);
    mlme.bss.ies = saved_ies;
    mlme.bss.ies_len = saved_len;
    mlme.bw_limit = bw_limit;
    mlme.ht = ht;
    mlme.vht = vht;
    mlme.he = he;
}

/* The elements a change of which matters after the join (care_about_ies in
 * Linux), reduced to one number. */
static u32 mlme_beacon_hash(const struct ieee80211_mgmt *mgmt, const u8 *ies, size_t len)
{
    const u8 *fixed = (const u8 *)&mgmt->u.beacon.beacon_int;   /* interval, capabilities */
    const struct element *elem;
    u32 hash = 2166136261u;     /* FNV-1a */
    unsigned int i;

    for (i = 0; i < 4; i++)
        hash = (hash ^ fixed[i]) * 16777619u;

    for_each_element(elem, ies, len) {
        const u8 *raw = (const u8 *)elem;
        bool care;

        switch (elem->id) {
        case WLAN_EID_ERP_INFO:
        case WLAN_EID_HT_OPERATION:
        case WLAN_EID_VHT_OPERATION:
        case WLAN_EID_CHANNEL_SWITCH:
        case WLAN_EID_EXT_CHANSWITCH_ANN:
            care = true;
            break;
        case WLAN_EID_VENDOR_SPECIFIC:  /* WMM parameters */
            care = elem->datalen >= 7 && elem->data[0] == 0x00 && elem->data[1] == 0x50 &&
                   elem->data[2] == 0xf2 && elem->data[3] == WLAN_OUI_TYPE_MICROSOFT_WMM &&
                   elem->data[4] == 1;
            break;
        case WLAN_EID_EXTENSION:
            care = elem->datalen >= 1 && (elem->data[0] == WLAN_EID_EXT_HE_OPERATION ||
                                          elem->data[0] == WLAN_EID_EXT_HE_MU_EDCA);
            break;
        default:
            care = false;
            break;
        }
        for (i = 0; care && i < 2u + elem->datalen; i++)
            hash = (hash ^ raw[i]) * 16777619u;
    }
    return hash;
}

/* ieee80211_rx_mgmt_beacon(), as far as this MLME goes. */
static void mlme_rx_beacon(const struct ieee80211_mgmt *mgmt, size_t len, u16 freq)
{
    const size_t fixed = offsetof(struct ieee80211_mgmt, u.beacon.variable);
    struct ieee80211_bss_conf *conf = &mlme.vif->bss_conf;
    const struct ieee80211_mu_edca_param_set *mu_edca = NULL;
    const struct element *elem, *wmm;
    struct cfg80211_chan_def def;
    const u8 *ies;
    size_t ies_len;
    u64 changed = 0;
    u32 hash;

    if (mlme.state < RTW89_MLME_ASSOCIATED || len < fixed)
        return;
    /* ieee80211_rx_beacon_freq_valid(): heard while tuned elsewhere, as a
     * scan does, it is not evidence of anything */
    if (freq != mlme.chan->center_freq)
        return;
    ies = mgmt->u.beacon.variable;
    ies_len = len - fixed;

    mlme.beacons++;
    mlme.last_beacon = jiffies;
    if (mlme.poll) {
        mlme_info("a beacon arrived: still connected");
        mlme_reset_ap_probe();
    }

    hash = mlme_beacon_hash(mgmt, ies, ies_len);
    if (mlme.beacon_hash_valid && hash == mlme.beacon_hash)
        return;
    mlme.beacon_hash = hash;
    mlme.beacon_hash_valid = true;

    mlme_beacon_chanswitch(ies, ies_len);

    changed |= mlme_bss_capability(le16_to_cpu(mgmt->u.beacon.capab_info),
                                   mlme_find_elem(WLAN_EID_ERP_INFO, ies, ies_len));

    /* new EDCA parameters: the AP counts the versions */
    wmm = mlme.sta->wme ? mlme_wmm_param(ies, ies_len) : NULL;
    if (wmm) {
        int count = wmm->data[6] & 0x0f, mu_count = -1;

        if (conf->he_support)
            mu_edca = (const void *)mlme_ext_elem(WLAN_EID_EXT_HE_MU_EDCA, ies, ies_len,
                                                  sizeof(*mu_edca), NULL);
        if (mu_edca)
            mu_count = mu_edca->mu_qos_info & 0x0f;
        if (count != mlme.wmm_param_set || mu_count != mlme.mu_edca_param_set) {
            mlme_conf_tx(wmm, mu_edca);
            changed |= BSS_CHANGED_QOS;
        }
    }

    /* ieee80211_config_bw(): the protection the AP asks 802.11n stations for */
    elem = mlme.ht ? mlme_find_elem(WLAN_EID_HT_OPERATION, ies, ies_len) : NULL;
    if (elem && elem->datalen >= sizeof(struct ieee80211_ht_operation)) {
        u16 mode = le16_to_cpu(((const struct ieee80211_ht_operation *)elem->data)->operation_mode);

        if (conf->ht_operation_mode != mode) {
            conf->ht_operation_mode = mode;
            changed |= BSS_CHANGED_HT;
        }
    }

    /*
     * ... and the channel itself. Linux changes width with the AP; here a
     * change ends the connection, to be joined again as the AP now is. The
     * first beacon is the reference, not the scan result the join used, so
     * that an AP whose beacons and probe responses disagree is not left
     * over and over.
     */
    mlme_beacon_chandef(ies, ies_len, &def);
    if (!mlme.beacon_def_valid) {
        mlme.beacon_def = def;
        mlme.beacon_def_valid = true;
        if (def.width != mlme.chandef.width || def.center_freq1 != mlme.chandef.center_freq1)
            mlme_info("the beacon describes a %u MHz channel around %u MHz, the scan said %u around %u",
                      mlme_chandef_mhz(&def), def.center_freq1,
                      mlme_chandef_mhz(&mlme.chandef), mlme.chandef.center_freq1);
    } else if (!mlme.csa_pending && (def.width != mlme.beacon_def.width ||
                                     def.center_freq1 != mlme.beacon_def.center_freq1)) {
        mlme_info("the access point changed its channel to %u MHz around %u MHz: leaving to join again",
                  mlme_chandef_mhz(&def), def.center_freq1);
        mlme.last_error = -ENETRESET;
        mlme_teardown(WLAN_REASON_DEAUTH_LEAVING);
        return;
    }

    if (changed) {
        mlme.beacon_updates++;
        if (mlme.local->ops->link_info_changed)
            mlme.local->ops->link_info_changed(mlme.hw, mlme.vif, conf, changed);
    }
}

static void mlme_rx_frame(struct sk_buff *skb)
{
    const struct ieee80211_mgmt *mgmt = (const void *)skb->data;
    __le16 fc = mgmt->frame_control;
    size_t len = skb->len;

    if (mlme.state == RTW89_MLME_IDLE || len < sizeof(struct ieee80211_hdr_3addr))
        return;

    if (!ieee80211_is_mgmt(fc) || !ether_addr_equal(mgmt->bssid, mlme.bss.bssid))
        return;
    if (ieee80211_is_beacon(fc)) {
        mlme_rx_beacon(mgmt, len, IEEE80211_SKB_RXCB(skb)->freq);
        return;
    }
    if (!ether_addr_equal(mgmt->da, mlme.vif->addr))
        return;

    /* 802.11w: robust frames to us come protected once the keys are in */
    if (ieee80211_has_protected(fc)) {
        if (ieee80211_is_auth(fc) || !mlme_unprotect(skb))
            return;
        mgmt = (const void *)skb->data;
        fc = mgmt->frame_control;
        len = skb->len;
    } else if (mlme.mfp && mlme.ptk_conf && mlme_robust(skb->data, len)) {
        if ((ieee80211_is_deauth(fc) || ieee80211_is_disassoc(fc)) &&
            len >= offsetof(struct ieee80211_mgmt, u.deauth) + 2)
            mlme_sa_query_start(le16_to_cpu(mgmt->u.deauth.reason_code));
        return;
    }

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
/* A beacon, FCS still on; IEEE80211_SKB_RXCB(skb) says where it was heard.
 * Takes the skb. Any context. */
void rtw89_mlme_rx_beacon(struct sk_buff *skb)
{
    const struct ieee80211_mgmt *mgmt = (const void *)skb->data;

    /* only the AP's own, and not more than can be looked at */
    if (!mlme.running || mlme.state < RTW89_MLME_ASSOCIATED ||
        skb->len < sizeof(struct ieee80211_hdr_3addr) + FCS_LEN ||
        !ether_addr_equal(mgmt->bssid, mlme.bss.bssid) || skb_queue_len(&mlme.rxq) > 16) {
        kfree_skb(skb);
        return;
    }
    rtw89_mlme_rx(skb);
}

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
        if (mlme.tries < MLME_MAX_TRIES)
            mlme_send_auth();
        else if (mlme.sae && mlme.sae_confirm_sent)
            /* an AP with another password drops our confirm without a word */
            mlme_fail(-2000 - WLAN_REASON_PREV_AUTH_NOT_VALID,
                      "no SAE confirm from the AP: wrong password?");
        else if (!mlme_sae_fall_back("no answer to the SAE commit"))
            mlme_fail(-ETIMEDOUT, "no answer to the authentication request");
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

void rtw89_mlme_set_he(bool on)
{
    mlme_he_off = !on;
}

bool rtw89_mlme_get_he(void)
{
    return !mlme_he_off;
}

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
    wiphy_work_init(&mlme.beacon_loss_work, mlme_beacon_loss_work);
    wiphy_work_init(&mlme.probe_work, mlme_probe_work);
    wiphy_delayed_work_init(&mlme.probe_timeout_work, mlme_probe_work);
    wiphy_delayed_work_init(&mlme.bcn_mon_work, mlme_bcn_mon_work);
    wiphy_delayed_work_init(&mlme.csa_work, mlme_csa_work);
    mlme.wmm_param_set = -1;
    mlme.mu_edca_param_set = -1;
    wiphy_work_init(&mlme.ba_work, mlme_ba_work);
    wiphy_delayed_work_init(&mlme.ba_timeout_work, mlme_ba_timeout_work);
    wiphy_delayed_work_init(&mlme.timeout_work, mlme_timeout_work);
    wiphy_delayed_work_init(&mlme.sa_query_work, mlme_sa_query_work);
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
    wiphy_work_cancel(mlme.hw->wiphy, &mlme.beacon_loss_work);
    wiphy_work_cancel(mlme.hw->wiphy, &mlme.probe_work);
    wiphy_delayed_work_cancel(mlme.hw->wiphy, &mlme.probe_timeout_work);
    wiphy_delayed_work_cancel(mlme.hw->wiphy, &mlme.bcn_mon_work);
    wiphy_delayed_work_cancel(mlme.hw->wiphy, &mlme.csa_work);
    wiphy_delayed_work_cancel(mlme.hw->wiphy, &mlme.sa_query_work);
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
        /* once associated, what the AP's answer really gave us */
        if (mlme.sta && status->state >= RTW89_MLME_ASSOCIATED)
            status->mode = mlme.sta->deflink.he_cap.has_he ? 3 :
                           mlme.sta->deflink.vht_cap.vht_supported ? 2 :
                           mlme.sta->deflink.ht_cap.ht_supported ? 1 : 0;
        else
            status->mode = mlme.he ? 3 : mlme.vht ? 2 : mlme.ht ? 1 : 0;
        status->nss = mlme.sta ? mlme.sta->deflink.rx_nss : 0;
    }
    status->beacons = mlme.beacons;
    status->beacon_losses = mlme.beacon_losses;
    status->beacon_updates = mlme.beacon_updates;
    status->probe_acks = mlme.probe_acks;
    if (status->state >= RTW89_MLME_ASSOCIATED) {
        struct ieee80211_sta *sta = mlme.sta;   /* freed late, see rtw89_m80211_free_later() */
        struct station_info *sinfo;
        unsigned int tid;

        /* the rate the firmware's rate control is sending at (the structure
         * is too big for a kernel stack) */
        sinfo = sta && mlme.local->ops->sta_statistics ? kzalloc(sizeof(*sinfo), GFP_KERNEL) : NULL;
        if (sinfo) {
            mlme.local->ops->sta_statistics(mlme.hw, mlme.vif, sta, sinfo);
            if (sinfo->filled & BIT_ULL(NL80211_STA_INFO_TX_BITRATE)) {
                status->tx_rate = sinfo->txrate;
                status->tx_rate_valid = true;
            }
            kfree(sinfo);
        }

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
static int mlme_connect(const struct rtw89_mlme_bss *bss, const u8 *pmk,
                        const u8 *password, size_t password_len, bool external,
                        const u8 *rsn_ie, size_t rsn_len);

/* Join a network with a shared key: @pmk for WPA2-PSK, @password for SAE
 * (WPA3-Personal). Either may be missing; open networks need neither. */
int rtw89_mlme_connect(const struct rtw89_mlme_bss *bss, const u8 *pmk,
                       const u8 *password, size_t password_len)
{
    return mlme_connect(bss, pmk, password, password_len, false, NULL, 0);
}

/* Join a network whose sign-in and key handshake an outside supplicant does.
 * @rsn_ie: the RSN element it wants in the association request, if it has one. */
int rtw89_mlme_connect_ext(const struct rtw89_mlme_bss *bss, const u8 *rsn_ie, size_t rsn_len)
{
    return mlme_connect(bss, NULL, NULL, 0, true, rsn_ie, rsn_len);
}

/*
 * A key from the outside supplicant: the pairwise key, or the group key with
 * id @idx whose receive counter stands at @rsc. With both in place the
 * station is authorised and data flows.
 */
int rtw89_mlme_set_key(bool pairwise, int idx, const u8 *key, size_t len, u64 rsc)
{
    if (!mlme.running || !mlme.external || !mlme.sta || mlme.state != RTW89_MLME_CONNECTED)
        return -ENOLINK;
    if (len != WLAN_KEY_LEN_CCMP || idx < 0 || idx > 3)
        return -EOPNOTSUPP;

    if (pairwise) {
        if (!mlme_key_is(mlme.ptk_conf, key, 0)) {
            rtw89_data_set_tx_key(NULL);
            rtw89_data_set_rx_key(-1, false, 0);
            mlme_key_remove(&mlme.ptk_conf, mlme.sta);
            if (mlme_key_install(&mlme.ptk_conf, mlme.sta, key, 0))
                return -EIO;
            rtw89_data_set_rx_key(-1, true, 0);
            rtw89_data_set_tx_key(mlme.ptk_conf);
        }
    } else if (mlme_gtk_install(idx, key, rsc)) {
        return -EIO;
    }

    if (mlme.sta_state < IEEE80211_STA_AUTHORIZED && mlme.ptk_conf &&
        (mlme.gtk_conf[0] || mlme.gtk_conf[1] || mlme.gtk_conf[2] || mlme.gtk_conf[3])) {
        if (mlme_sta_move(IEEE80211_STA_AUTHORIZED))
            return -EIO;
        rtw89_data_authorize();
        mlme_info("keys from the system's supplicant installed: connected");
        if (mlme.notify)
            mlme.notify();
    }
    return 0;
}

/*
 * The pairwise master key the outside supplicant's sign-in produced (802.1X:
 * the first 32 bytes of the MSK). From here the key handshake is ours, as
 * with a shared password. A later one (the sign-in was repeated) replaces it.
 */
int rtw89_mlme_set_pmk(const u8 *pmk, size_t len)
{
    u8 held[sizeof(mlme.held_m1)];
    size_t held_len;

    if (!mlme.running || !mlme.external || !mlme.sta || mlme.state != RTW89_MLME_CONNECTED)
        return -ENOLINK;
    if (len != sizeof(mlme.pmk))
        return -EINVAL;
    memcpy(mlme.pmk, pmk, sizeof(mlme.pmk));
    mlme.have_pmk = true;
    mlme.snonce_valid = false;
    mlme_info("sign-in done, master key received%s", mlme.held_m1_len ?
              ": answering the AP's key handshake" : "");

    held_len = mlme.held_m1_len;
    mlme.held_m1_len = 0;
    if (held_len) {
        memcpy(held, mlme.held_m1, held_len);
        mlme_rx_eapol(held, held_len);
    }
    return 0;
}

/* The RSN element of the association request of the current or last join.
 * Returns its length, 0 for an open network. */
size_t rtw89_mlme_assoc_rsn_ie(u8 *buf, size_t max)
{
    const u8 *ie = mlme.external ? mlme.ext_rsn_ie : mlme.own_rsn_ie;
    size_t len = mlme.external ? mlme.ext_rsn_len : mlme.own_rsn_len;

    if (!mlme.running || mlme.state == RTW89_MLME_IDLE || !mlme.rsn || len > max)
        return 0;
    memcpy(buf, ie, len);
    return len;
}

bool rtw89_mlme_authorized(void)
{
    return mlme.running && mlme.sta && mlme.sta_state == IEEE80211_STA_AUTHORIZED;
}

static int mlme_connect(const struct rtw89_mlme_bss *bss, const u8 *pmk,
                        const u8 *password, size_t password_len, bool external,
                        const u8 *rsn_ie, size_t rsn_len)
{
    const struct ieee80211_ops *ops = mlme.local->ops;
    struct ieee80211_bss_conf *conf = &mlme.vif->bss_conf;
    const struct ieee80211_he_operation *he_oper;
    struct cfg80211_chan_def chandef;
    const struct element *elem;
    const u8 *he_cap;
    u32 rates, basic;
    u8 he_cap_len;
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
    mlme.akm = 0;
    mlme.mfp = false;

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
    /* 802.11ax: on top of those */
    he_cap = mlme_he_cap_ie(mlme.ies, bss->ies_len, &he_cap_len);
    he_oper = mlme_he_oper_ie(mlme.ies, bss->ies_len);
    mlme.he = mlme.ht && !mlme_he_off && mlme_own_he_cap() && he_cap && he_oper;
    mlme_determine_chandef(&mlme.chandef);
    if (mlme.he && !mlme_he_mcs_ok(he_cap, he_oper)) {
        mlme_info("the AP's 802.11ax rates are inconsistent or beyond this card: not using 802.11ax");
        mlme.he = false;
    }

    elem = mlme_find_elem(WLAN_EID_RSN, mlme.ies, bss->ies_len);
    mlme.rsn = elem != NULL;
    mlme.have_pmk = false;
    mlme.held_m1_len = 0;
    mlme.external = external && elem;
    mlme.sae_fallback = false;
    if (elem && external) {
        ret = mlme_check_rsn_8021x(elem);
        if (ret)
            return ret;
        /* its element as it is, if it is one: the handshake repeats it */
        if (rsn_ie && rsn_len >= 2 && rsn_len <= sizeof(mlme.ext_rsn_ie) &&
            rsn_ie[0] == WLAN_EID_RSN && (size_t)rsn_ie[1] + 2 == rsn_len) {
            memcpy(mlme.ext_rsn_ie, rsn_ie, rsn_len);
            mlme.ext_rsn_len = (u8)rsn_len;
        } else {
            memcpy(mlme.ext_rsn_ie, mlme_rsn_ie_8021x, sizeof(mlme_rsn_ie_8021x));
            mlme.ext_rsn_len = sizeof(mlme_rsn_ie_8021x);
        }
    } else if (elem) {
        ret = mlme_parse_rsn(elem);
        if (ret)
            return ret;
        if (!pmk && !password_len) {
            mlme_info("the network needs a password");
            return -EACCES;
        }
        ret = mlme_choose_akm(password_len > 0, pmk != NULL);
        if (ret)
            return ret;
        if (pmk)
            memcpy(mlme.pmk, pmk, sizeof(mlme.pmk));
        if (mlme.akm == AKM_SAE)
            mlme.sae_fallback = pmk && (mlme.ap_akms & (BIT(AKM_PSK) | BIT(AKM_PSK_SHA256)));
        else
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

    mlme_info("joining \"%.*s\" (%02x:%02x:%02x:%02x:%02x:%02x) on %u MHz, %u MHz wide%s%s%s",
              bss->ssid_len, bss->ssid, bss->bssid[0], bss->bssid[1], bss->bssid[2],
              bss->bssid[3], bss->bssid[4], bss->bssid[5], bss->freq,
              mlme_chandef_mhz(&mlme.chandef),
              mlme.he ? ", 802.11ax" : mlme.vht ? ", 802.11ac" : mlme.ht ? ", 802.11n" : "",
              !mlme.rsn ? ", open" : mlme.external ? ", WPA2 with 802.1X sign-in" :
              mlme.akm == AKM_SAE ? ", WPA3 (SAE)" :
              mlme.akm == AKM_PSK_SHA256 ? ", WPA2 (PSK-SHA256)" : ", WPA2",
              mlme.mfp ? ", protected management frames" : "");

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
    rtw89_data_set_external(mlme.external);

    mlme.eapol_reports = 0;
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
    /* drv_assign_vif_chanctx(): absent when rtw89 emulates channel contexts
     * (RTL8851B, RTL8852A: firmware without beacon filtering), where
     * add_chanctx has already set the channel through ->config() */
    ret = ops->assign_vif_chanctx ?
          ops->assign_vif_chanctx(mlme.hw, mlme.vif, conf, mlme.chanctx) : 0;
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

    if (mlme.rsn && !mlme.external && mlme.akm == AKM_SAE) {
        /* the password element and our commit: the expensive part, once */
        mlme.sae = rtw89_sae_begin(mlme.vif->addr, bss->bssid, password, password_len);
        if (!mlme.sae) {
            mlme_info("could not start SAE with this password");
            ret = -EINVAL;
            goto err;
        }
    }
    mlme_send_auth();
    return 0;

err:
    mlme.last_error = ret;
    mlme_teardown(0);
    return ret;
}
