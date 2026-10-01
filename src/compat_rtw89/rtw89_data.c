// SPDX-License-Identifier: GPL-2.0
/*
 * Data path of the station interface.
 *
 * Ethernet frames from the network stack become 802.11 data frames on the
 * station's TX queues; received 802.11 data frames become Ethernet frames.
 * This is what mac80211 does in tx.c (ieee80211_build_hdr and the sequence and
 * CCMP handlers) and rx.c (the duplicate, decrypt, A-MSDU and 802.3 conversion
 * handlers), reduced to one non-MLO station interface with CCMP done by the
 * hardware.
 *
 * Frames of a BlockAck session arrive as the chip received them, with gaps
 * where a subframe has to be sent again; the reorder buffer here puts them
 * back in order (ieee80211_sta_manage_reorder_buf() and friends). The
 * sessions themselves are negotiated by rtw89_mlme.c.
 *
 * Not here yet: fragments (dropped) and software decryption (a protected
 * frame the hardware did not decrypt is dropped and counted).
 *
 * Locking: tx_lock orders sequence numbers, packet numbers and the queueing of
 * a frame. rx_lock serialises the whole receive path, delivery to the network
 * stack included, between the receive thread and the reorder timers (what
 * rx_path_lock does in mac80211). The MLME changes the peer, the keys and the
 * sessions with the wiphy mutex held and takes these locks to do it, so the
 * network stack's threads never need the wiphy mutex. Order: rx_lock, then
 * tx_lock.
 */
#include "rtw89_net80211.h"

#define DATA_MAX_FRAME      (ETH_HLEN + IEEE80211_MAX_DATA_LEN)
/* 802.11 QoS header, CCMP header and LLC/SNAP in front of the payload */
#define DATA_TX_HEADROOM    (sizeof(struct ieee80211_qos_hdr) + IEEE80211_CCMP_HDR_LEN + 8)
#define DATA_TXQ_LIMIT      512         /* frames waiting per TID before new ones are dropped */
#define DATA_NUM_TIDS       (IEEE80211_NUM_TIDS + 1)    /* the last one: frames without QoS */
#define DATA_NOQOS          IEEE80211_NUM_TIDS

#ifndef ETH_P_802_3_MIN
#define ETH_P_802_3_MIN     0x0600
#endif
#define DATA_ETH_P_AARP     0x80F3
#define DATA_ETH_P_IPX      0x8137

#define DATA_REORDER_MAX    IEEE80211_MAX_AMPDU_BUF_HT
#define DATA_REORDER_TIMEOUT (HZ / 10)  /* HT_RX_REORDER_BUF_TIMEOUT */

/* Receive state of one key: the last packet number seen per TID. */
struct data_rx_key {
    bool valid;
    u64 pn[DATA_NUM_TIDS];
};

/* The receive side of a BlockAck session: frames waiting for the ones before
 * them. slot[sn % buf_size] holds the frame with sequence number sn. */
struct data_rx_ba {
    bool active;
    bool started;               /* a frame at or after the start has been seen */
    u16 head_sn;                /* the next sequence number to hand on */
    u16 buf_size;
    u16 stored;
    struct sk_buff *slot[DATA_REORDER_MAX];
    unsigned long time[DATA_REORDER_MAX];
    struct timer_list timer;    /* gives up waiting for a missing frame */
};

static struct {
    spinlock_t tx_lock;
    spinlock_t rx_lock;
    bool inited;

    struct ieee80211_hw *hw;
    void (*deliver)(const u8 *frame, size_t len);
    void (*tx_tap)(const u8 *frame, size_t len);

    /* the access point; sta is NULL while not associated */
    struct ieee80211_vif *vif;
    struct ieee80211_sta *sta;
    u8 bssid[ETH_ALEN];
    u8 addr[ETH_ALEN];
    enum nl80211_band band;
    bool protect;               /* the network encrypts: never send or accept data in the clear */
    bool authorized;            /* the key handshake is done (or there is none) */

    struct ieee80211_key_conf *tx_key;
    u16 tx_sn[DATA_NUM_TIDS];

    struct data_rx_key rx_ptk;
    struct data_rx_key rx_gtk[4];
    u16 rx_last_sc[DATA_NUM_TIDS];
    bool rx_sc_valid[DATA_NUM_TIDS];
    struct data_rx_ba rx_ba[IEEE80211_NUM_TIDS];

    struct rtw89_data_stats stats;
} data;

static const u8 data_pae_group_addr[ETH_ALEN] = { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x03 };

/* 802.1D priority (TID & 7) to access category, as ieee802_1d_to_ac[] */
static const u8 data_tid_to_ac[8] = {
    IEEE80211_AC_BE, IEEE80211_AC_BK, IEEE80211_AC_BK, IEEE80211_AC_BE,
    IEEE80211_AC_VI, IEEE80211_AC_VI, IEEE80211_AC_VO, IEEE80211_AC_VO,
};

/* ------------------------------------------------------------------ */
/*  Setup: called by the glue and the MLME                              */
/* ------------------------------------------------------------------ */

static void data_rx_reorder_timer(struct timer_list *t);

void rtw89_data_init(struct ieee80211_hw *hw, void (*deliver)(const u8 *frame, size_t len))
{
    void (*tap)(const u8 *, size_t) = data.tx_tap;
    int i;

    memset(&data, 0, sizeof(data));
    data.tx_tap = tap;
    spin_lock_init(&data.tx_lock);
    spin_lock_init(&data.rx_lock);
    for (i = 0; i < IEEE80211_NUM_TIDS; i++)
        timer_setup(&data.rx_ba[i].timer, data_rx_reorder_timer, 0);
    data.hw = hw;
    data.deliver = deliver;
    data.inited = true;
}

void rtw89_data_exit(void)
{
    if (!data.inited)
        return;
    rtw89_data_detach();
    data.inited = false;
    data.hw = NULL;
    spin_lock_destroy(&data.tx_lock);
    spin_lock_destroy(&data.rx_lock);
}

void rtw89_data_set_tx_tap(void (*tap)(const u8 *frame, size_t len))
{
    data.tx_tap = tap;
}

void rtw89_data_attach(struct ieee80211_vif *vif, struct ieee80211_sta *sta,
                       const u8 *bssid, enum nl80211_band band, bool protect)
{
    spin_lock(&data.rx_lock);
    spin_lock(&data.tx_lock);
    data.vif = vif;
    memcpy(data.bssid, bssid, ETH_ALEN);
    memcpy(data.addr, vif->addr, ETH_ALEN);
    data.band = band;
    data.protect = protect;
    data.authorized = false;
    data.tx_key = NULL;
    memset(data.tx_sn, 0, sizeof(data.tx_sn));
    memset(&data.rx_ptk, 0, sizeof(data.rx_ptk));
    memset(data.rx_gtk, 0, sizeof(data.rx_gtk));
    memset(data.rx_sc_valid, 0, sizeof(data.rx_sc_valid));
    data.sta = sta;
    spin_unlock(&data.tx_lock);
    spin_unlock(&data.rx_lock);
}

/* After this returns no frame is being built for the station and none is
 * left on its queues; received frames are dropped. */
void rtw89_data_detach(void)
{
    struct ieee80211_sta *sta;
    int i;

    if (!data.inited)
        return;

    for (i = 0; i < IEEE80211_NUM_TIDS; i++)
        rtw89_data_rx_ba_stop((u8)i);

    spin_lock(&data.rx_lock);
    spin_lock(&data.tx_lock);
    sta = data.sta;
    data.sta = NULL;
    data.authorized = false;
    data.tx_key = NULL;
    data.rx_ptk.valid = false;
    memset(data.rx_gtk, 0, sizeof(data.rx_gtk));
    spin_unlock(&data.tx_lock);
    spin_unlock(&data.rx_lock);

    if (sta)
        rtw89_m80211_sta_purge_txqs(sta);
}

void rtw89_data_authorize(void)
{
    spin_lock(&data.rx_lock);
    spin_lock(&data.tx_lock);
    data.authorized = data.sta != NULL;
    spin_unlock(&data.tx_lock);
    spin_unlock(&data.rx_lock);
}

/* The pairwise key to send with from now on; NULL stops data until the next. */
void rtw89_data_set_tx_key(struct ieee80211_key_conf *key)
{
    spin_lock(&data.tx_lock);
    data.tx_key = key;
    spin_unlock(&data.tx_lock);
}

/* A receive key was installed (@valid) or removed. @keyidx: -1 for the
 * pairwise key, else the group key id. @rsc: the highest packet number the AP
 * has already used with it. */
void rtw89_data_set_rx_key(int keyidx, bool valid, u64 rsc)
{
    struct data_rx_key *key;
    int i;

    if (keyidx > 3)
        return;
    key = keyidx < 0 ? &data.rx_ptk : &data.rx_gtk[keyidx];

    spin_lock(&data.rx_lock);
    key->valid = valid;
    for (i = 0; i < DATA_NUM_TIDS; i++)
        key->pn[i] = rsc;
    spin_unlock(&data.rx_lock);
}

void rtw89_data_get_stats(struct rtw89_data_stats *stats)
{
    *stats = data.stats;
}

/* ------------------------------------------------------------------ */
/*  Transmit                                                            */
/* ------------------------------------------------------------------ */

/* An skb for one Ethernet frame of @len bytes: the caller fills skb->data and
 * passes it to rtw89_data_tx(). The headers are built in the room in front. */
struct sk_buff *rtw89_data_tx_alloc(size_t len)
{
    struct ieee80211_hw *hw = data.hw;
    unsigned int headroom;
    struct sk_buff *skb;

    if (!hw || len < ETH_HLEN || len > DATA_MAX_FRAME)
        return NULL;

    headroom = hw->extra_tx_headroom + DATA_TX_HEADROOM;
    skb = dev_alloc_skb((u32)(headroom + len));
    if (!skb)
        return NULL;
    skb_reserve(skb, headroom);
    skb_put(skb, (u32)len);
    return skb;
}

/* cfg80211_classify8021d() for the cases without a QoS map: the top three bits
 * of the DSCP field. */
static u8 data_classify(u16 type, const u8 *payload, size_t len)
{
    if (type == ETH_P_IP && len >= 2)
        return payload[1] >> 5;
    if (type == ETH_P_IPV6 && len >= 2)
        return (payload[0] & 0x0f) >> 1;
    return 0;
}

/* Takes the skb. Returns 0 if the frame was queued. */
int rtw89_data_tx(struct sk_buff *skb)
{
    struct ieee80211_key_conf *key;
    struct ieee80211_tx_info *info;
    struct ieee80211_qos_hdr *hdr;
    struct ieee80211_txq *txq;
    unsigned long depth = 0;
    u8 da[ETH_ALEN], sa[ETH_ALEN];
    unsigned int hdrlen, sn_idx;
    const u8 *payload;
    size_t payload_len;
    bool eapol, qos;
    u16 type, fc;
    u8 tid, ac;
    int ret;

    if (!data.inited || skb->len < ETH_HLEN) {
        kfree_skb(skb);
        return -EINVAL;
    }

    memcpy(da, skb->data, ETH_ALEN);
    memcpy(sa, skb->data + ETH_ALEN, ETH_ALEN);
    type = get_unaligned_be16(skb->data + 2 * ETH_ALEN);
    payload = skb->data + ETH_HLEN;
    payload_len = skb->len - ETH_HLEN;
    eapol = type == ETH_P_PAE;

    spin_lock(&data.tx_lock);

    /* the key handshake itself is the only thing that goes out before it is done */
    if (!data.sta || (!eapol && !data.authorized)) {
        ret = -ENETDOWN;
        goto drop;
    }
    if (!ether_addr_equal(sa, data.addr)) {
        ret = -EINVAL;
        goto drop;
    }
    key = data.tx_key;
    if (!eapol && data.protect && !key) {
        ret = -ENETDOWN;
        goto drop;
    }

    /* known once the AP has answered the association request */
    qos = data.sta->wme;
    tid = 0;
    if (qos)
        tid = eapol ? 7 : data_classify(type, payload, payload_len);
    ac = data_tid_to_ac[tid];
    sn_idx = qos ? tid : DATA_NOQOS;

    txq = data.sta->txq[tid];
    depth = skb_queue_len(&to_mtxq(txq)->frames);
    if (depth >= DATA_TXQ_LIMIT) {
        ret = -ENOBUFS;
        goto drop;
    }

    /*
     * Ethernet II: the type stays where it is, with the SNAP header in front
     * of it instead of the addresses. An 802.3 frame (a length, then LLC)
     * loses the whole Ethernet header.
     */
    if (type >= ETH_P_802_3_MIN) {
        skb_pull(skb, 2 * ETH_ALEN);
        memcpy(skb_push(skb, sizeof(rfc1042_header)),
               type == DATA_ETH_P_AARP || type == DATA_ETH_P_IPX ?
               bridge_tunnel_header : rfc1042_header, sizeof(rfc1042_header));
    } else {
        skb_pull(skb, ETH_HLEN);
    }

    if (key && (key->flags & (IEEE80211_KEY_FLAG_GENERATE_IV |
                              IEEE80211_KEY_FLAG_PUT_IV_SPACE))) {
        u8 *ccmp = skb_push(skb, IEEE80211_CCMP_HDR_LEN);

        memset(ccmp, 0, IEEE80211_CCMP_HDR_LEN);
        if (key->flags & IEEE80211_KEY_FLAG_GENERATE_IV) {
            /* the next packet number; the hardware encrypts and adds the MIC */
            u64 pn = (u64)atomic64_inc_return(&key->tx_pn);

            ccmp[0] = (u8)pn;
            ccmp[1] = (u8)(pn >> 8);
            ccmp[3] = 0x20 | (u8)(key->keyidx << 6);    /* extended IV, key id */
            ccmp[4] = (u8)(pn >> 16);
            ccmp[5] = (u8)(pn >> 24);
            ccmp[6] = (u8)(pn >> 32);
            ccmp[7] = (u8)(pn >> 40);
        }
    }

    hdrlen = qos ? sizeof(struct ieee80211_qos_hdr) : sizeof(struct ieee80211_hdr_3addr);
    hdr = skb_push(skb, hdrlen);
    memset(hdr, 0, hdrlen);
    fc = IEEE80211_FTYPE_DATA | IEEE80211_FCTL_TODS |
         (qos ? IEEE80211_STYPE_QOS_DATA : IEEE80211_STYPE_DATA);
    if (key)
        fc |= IEEE80211_FCTL_PROTECTED;
    hdr->frame_control = cpu_to_le16(fc);
    memcpy(hdr->addr1, data.bssid, ETH_ALEN);       /* receiver: the AP */
    memcpy(hdr->addr2, sa, ETH_ALEN);               /* transmitter and source */
    memcpy(hdr->addr3, da, ETH_ALEN);               /* destination */
    hdr->seq_ctrl = cpu_to_le16((u16)((data.tx_sn[sn_idx]++ & 0xfff) << 4));
    if (qos)
        hdr->qos_ctrl = cpu_to_le16(tid);

    info = IEEE80211_SKB_CB(skb);
    memset(info, 0, sizeof(*info));
    info->control.vif = data.vif;
    info->control.hw_key = key;
    info->band = data.band;
    info->hw_queue = data.vif->hw_queue[ac];
    if (eapol)
        info->control.flags = IEEE80211_TX_CTRL_PORT_CTRL_PROTO;

    /* what the driver looks at to recognise ARP, DHCP and ICMP */
    skb->protocol = htons(type);
    skb->priority = tid;
    skb_set_queue_mapping(skb, ac);
    skb->network_header = (u16)(payload - skb->head);
    skb->transport_header = skb->network_header;
    if (type == ETH_P_IP) {
        size_t ihl = payload_len ? (payload[0] & 0x0f) * 4u : 0;

        if (ihl < 20 || payload_len < ihl + 8)
            skb->protocol = 0;                      /* not something to look into */
        else
            skb->transport_header = (u16)(skb->network_header + ihl);
    }

    if (data.tx_tap)
        data.tx_tap(skb->data, skb->len);
    rtw89_m80211_tx(data.hw, txq, skb);
    data.stats.tx_frames++;
    spin_unlock(&data.tx_lock);
    return 0;

drop:
    data.stats.tx_dropped++;
    spin_unlock(&data.tx_lock);
    kfree_skb(skb);
    return ret;
}

/* ------------------------------------------------------------------ */
/*  Transmit BlockAck sessions (for the MLME)                           */
/* ------------------------------------------------------------------ */

/*
 * A session is about to be negotiated for @tid: hold its frames back until
 * the AP has agreed, and return the sequence number the session starts at,
 * i.e. that of the first frame held back. (mac80211 stops the TXQ the same
 * way; frames of the session sent before the AP knows about it would leave
 * holes in its reorder window.)
 */
int rtw89_data_tx_ba_prepare(u8 tid, u16 *ssn)
{
    int ret = -ENETDOWN;

    spin_lock(&data.tx_lock);
    if (data.sta && data.sta->wme && tid < IEEE80211_NUM_TIDS) {
        unsigned int waiting = rtw89_m80211_txq_stop(data.sta->txq[tid]);

        *ssn = (u16)((data.tx_sn[tid] - waiting) & IEEE80211_SN_MASK);
        ret = 0;
    }
    spin_unlock(&data.tx_lock);
    return ret;
}

/* For tests: how many frames are waiting on @tid's queue. */
unsigned int rtw89_data_tx_waiting(u8 tid);
unsigned int rtw89_data_tx_waiting(u8 tid)
{
    unsigned int waiting = 0;

    spin_lock(&data.tx_lock);
    if (data.sta && tid < IEEE80211_NUM_TIDS)
        waiting = skb_queue_len(&to_mtxq(data.sta->txq[tid])->frames);
    spin_unlock(&data.tx_lock);
    return waiting;
}

void rtw89_data_tx_ba_resume(u8 tid)
{
    spin_lock(&data.tx_lock);
    if (data.sta && tid < IEEE80211_NUM_TIDS)
        rtw89_m80211_txq_start(data.hw, data.sta->txq[tid]);
    spin_unlock(&data.tx_lock);
}

/* ------------------------------------------------------------------ */
/*  Receive: one frame, in its final order                              */
/* ------------------------------------------------------------------ */

/* The lockless sk_buff_head of this file: frames ready to be processed. */
static struct sk_buff *data_frames_pop(struct sk_buff_head *frames)
{
    struct sk_buff *skb;

    if (skb_queue_empty(frames))
        return NULL;
    skb = skb_peek(frames);
    __skb_unlink(skb, frames);
    return skb;
}

/*
 * One MSDU: @p is its LLC header (or payload), with at least 14 writable
 * bytes in front of it. The Ethernet header is written there.
 */
static void data_rx_msdu(const u8 *da, const u8 *sa, u8 *p, size_t len, bool decrypted)
{
    u16 type = 0;
    size_t flen;
    u8 *eth;

    if (len >= 8)
        type = get_unaligned_be16(p + 6);
    if (len >= 8 &&
        ((!memcmp(p, rfc1042_header, 6) &&
          type != DATA_ETH_P_AARP && type != DATA_ETH_P_IPX) ||
         !memcmp(p, bridge_tunnel_header, 6))) {
        /* SNAP: the type is already in place, the addresses go in front of it */
        eth = p + 6 - 2 * ETH_ALEN;
        flen = len - 6 + 2 * ETH_ALEN;
    } else {
        /* anything else travels as an 802.3 frame with a length */
        type = 0;
        eth = p - ETH_HLEN;
        put_unaligned_be16((u16)len, eth + 2 * ETH_ALEN);
        flen = len + ETH_HLEN;
    }
    memcpy(eth, da, ETH_ALEN);
    memcpy(eth + ETH_ALEN, sa, ETH_ALEN);

    if (flen > DATA_MAX_FRAME)
        goto drop;

    if (type == ETH_P_PAE) {
        /* The key handshake: for the MLME, encrypted or not (as mac80211's
         * ieee80211_frame_allowed()), but only if addressed to us. */
        if (!ether_addr_equal(da, data.addr) && !ether_addr_equal(da, data_pae_group_addr))
            goto drop;
        rtw89_mlme_rx_eapol(eth + ETH_HLEN, flen - ETH_HLEN);
        return;
    }

    if (!data.authorized)
        goto drop;
    if (data.protect && !decrypted) {
        data.stats.rx_unprotected++;
        goto drop;
    }
    /* our own broadcasts, sent back by the AP */
    if (ether_addr_equal(sa, data.addr))
        goto drop;

    data.stats.rx_frames++;
    if (data.deliver)
        data.deliver(eth, flen);
    return;

drop:
    data.stats.rx_dropped++;
}

/* An A-MSDU: several MSDUs in one frame, each with its own little header. */
static void data_rx_amsdu(u8 *p, size_t len, bool decrypted)
{
    bool first = true;

    while (len > ETH_HLEN) {
        u16 sublen = get_unaligned_be16(p + 2 * ETH_ALEN);
        u8 da[ETH_ALEN], sa[ETH_ALEN];
        size_t padded;

        if (sublen > len - ETH_HLEN)
            break;
        memcpy(da, p, ETH_ALEN);
        memcpy(sa, p + ETH_ALEN, ETH_ALEN);

        /* A normal frame with the A-MSDU bit flipped starts with a SNAP header
         * where the destination should be: an injection attempt. */
        if (first && !memcmp(da, rfc1042_header, ETH_ALEN))
            break;
        first = false;

        if (is_multicast_ether_addr(da) || ether_addr_equal(da, data.addr))
            data_rx_msdu(da, sa, p + ETH_HLEN, sublen, decrypted);
        else
            data.stats.rx_dropped++;

        /* every subframe but the last is padded to a multiple of four */
        padded = ALIGN(ETH_HLEN + (size_t)sublen, 4);
        if (padded >= len)
            break;
        p += padded;
        len -= padded;
    }
}

/* What mac80211's rx handlers do after reordering: decryption checks, then
 * the payload. rx_lock is held; the caller frees the skb. */
static void data_rx_one(struct sk_buff *skb)
{
    const struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(skb);
    const struct ieee80211_hdr *hdr = (const void *)skb->data;
    __le16 fc = hdr->frame_control;
    unsigned int hdrlen = ieee80211_hdrlen(fc), tid = DATA_NOQOS;
    bool mcast = is_multicast_ether_addr(hdr->addr1);
    bool amsdu = false, decrypted = false;
    u8 da[ETH_ALEN], sa[ETH_ALEN];
    u8 *payload = skb->data + hdrlen;
    size_t len = skb->len - hdrlen;

    if (ieee80211_is_data_qos(fc)) {
        const u8 *qc = ieee80211_get_qos_ctl((struct ieee80211_hdr *)hdr);

        tid = qc[0] & IEEE80211_QOS_CTL_TID_MASK;
        amsdu = qc[0] & IEEE80211_QOS_CTL_A_MSDU_PRESENT;
    }

    if (ieee80211_has_morefrags(fc) || (le16_to_cpu(hdr->seq_ctrl) & IEEE80211_SCTL_FRAG)) {
        data.stats.rx_fragments++;
        goto drop;
    }

    if (ieee80211_has_protected(fc)) {
        struct data_rx_key *key;
        unsigned int keyid;
        u64 pn;

        /* Decrypted by the hardware, which leaves the CCMP header and MIC. */
        if (!(status->flag & RX_FLAG_DECRYPTED)) {
            data.stats.rx_undecrypted++;
            goto drop;
        }
        if (len < IEEE80211_CCMP_HDR_LEN + IEEE80211_CCMP_MIC_LEN || !(payload[3] & 0x20))
            goto drop;
        keyid = payload[3] >> 6;
        if (mcast)
            key = &data.rx_gtk[keyid];
        else if (!keyid)
            key = &data.rx_ptk;
        else
            goto drop;
        if (!key->valid)
            goto drop;

        /* the packet number must go up within each TID */
        pn = payload[0] | (u64)payload[1] << 8 | (u64)payload[4] << 16 |
             (u64)payload[5] << 24 | (u64)payload[6] << 32 | (u64)payload[7] << 40;
        if (!(status->flag & RX_FLAG_PN_VALIDATED)) {
            if (pn <= key->pn[tid]) {
                data.stats.rx_replay++;
                goto drop;
            }
            key->pn[tid] = pn;
        }

        payload += IEEE80211_CCMP_HDR_LEN;
        len -= IEEE80211_CCMP_HDR_LEN + IEEE80211_CCMP_MIC_LEN;
        decrypted = true;
    }

    /* the Ethernet header is written over the end of the 802.11 one */
    memcpy(da, hdr->addr1, ETH_ALEN);
    memcpy(sa, hdr->addr3, ETH_ALEN);

    if (!amsdu) {
        data_rx_msdu(da, sa, payload, len, decrypted);
    } else if (data.protect && !decrypted) {
        /* an A-MSDU is never an unencrypted EAPOL frame */
        data.stats.rx_unprotected++;
        data.stats.rx_dropped++;
    } else {
        data_rx_amsdu(payload, len, decrypted);
    }
    return;

drop:
    data.stats.rx_dropped++;
}

static void data_rx_frames(struct sk_buff_head *frames)
{
    struct sk_buff *skb;

    while ((skb = data_frames_pop(frames))) {
        data_rx_one(skb);
        kfree_skb(skb);
    }
}

/* ------------------------------------------------------------------ */
/*  Receive: the reorder buffer                                         */
/* ------------------------------------------------------------------ */

/* Hand on what is in @index (possibly nothing) and move the window past it. */
static void data_reorder_release_slot(struct data_rx_ba *ba, unsigned int index,
                                      struct sk_buff_head *frames)
{
    struct sk_buff *skb = ba->slot[index];

    if (skb) {
        ba->slot[index] = NULL;
        ba->stored--;
        __skb_queue_tail(frames, skb);
    }
    ba->head_sn = ieee80211_sn_inc(ba->head_sn);
}

/* ieee80211_release_reorder_frames(): everything before @head_sn goes on. */
static void data_reorder_release_until(struct data_rx_ba *ba, u16 head_sn,
                                       struct sk_buff_head *frames)
{
    while (ieee80211_sn_less(ba->head_sn, head_sn))
        data_reorder_release_slot(ba, ba->head_sn % ba->buf_size, frames);
}

/*
 * ieee80211_sta_reorder_release(): hand on the frames that are in order now.
 * A frame that never arrives must not hold up the ones behind it for ever:
 * once a waiting frame is DATA_REORDER_TIMEOUT old, the gap before it is
 * given up on.
 */
static void data_reorder_release(struct data_rx_ba *ba, struct sk_buff_head *frames)
{
    unsigned int index = ba->head_sn % ba->buf_size, j;

    if (!ba->slot[index] && ba->stored) {
        unsigned int skipped = 1;

        for (j = (index + 1) % ba->buf_size; j != index; j = (j + 1) % ba->buf_size) {
            if (!ba->slot[j]) {
                skipped++;
                continue;
            }
            if (skipped && !time_after(jiffies, ba->time[j] + DATA_REORDER_TIMEOUT))
                goto set_release_timer;

            /* move the window past the gap, then past this frame */
            ba->head_sn = (u16)((ba->head_sn + skipped) & IEEE80211_SN_MASK);
            data_reorder_release_slot(ba, j, frames);
            skipped = 0;
        }
    } else {
        while (ba->slot[index]) {
            data_reorder_release_slot(ba, index, frames);
            index = ba->head_sn % ba->buf_size;
        }
    }

    if (ba->stored) {
        /* wake up when the oldest frame still waiting has waited long enough */
        index = ba->head_sn % ba->buf_size;
        for (j = index; !ba->slot[j]; j = (j + 1) % ba->buf_size)
            ;
set_release_timer:
        mod_timer(&ba->timer, ba->time[j] + 1 + DATA_REORDER_TIMEOUT);
    } else {
        del_timer(&ba->timer);
    }
}

/*
 * ieee80211_rx_reorder_ampdu() + ieee80211_sta_manage_reorder_buf(): put @skb
 * on @frames, keep it for later, or drop it; frames it completes follow it.
 */
static void data_rx_reorder(struct sk_buff *skb, struct sk_buff_head *frames)
{
    const struct ieee80211_hdr *hdr = (const void *)skb->data;
    struct data_rx_ba *ba;
    unsigned int index;
    const u8 *qc;
    u16 sc, sn;

    if (!ieee80211_is_data_qos(hdr->frame_control) || is_multicast_ether_addr(hdr->addr1))
        goto pass;
    qc = ieee80211_get_qos_ctl((struct ieee80211_hdr *)hdr);
    ba = &data.rx_ba[qc[0] & IEEE80211_QOS_CTL_TID_MASK];
    if (!ba->active ||
        (qc[0] & IEEE80211_QOS_CTL_ACK_POLICY_MASK) == IEEE80211_QOS_CTL_ACK_POLICY_NOACK)
        goto pass;
    sc = le16_to_cpu(hdr->seq_ctrl);
    if (sc & IEEE80211_SCTL_FRAG)
        goto pass;
    sn = (sc & IEEE80211_SCTL_SEQ) >> 4;

    /* frames from before the session started are not part of it */
    if (!ba->started) {
        if (ieee80211_sn_less(sn, ba->head_sn))
            goto pass;
        ba->started = true;
    }

    /* behind the window: a retransmission of something already handed on */
    if (ieee80211_sn_less(sn, ba->head_sn))
        goto drop;

    /* beyond the window: the sender has moved on, so do we */
    if (!ieee80211_sn_less(sn, (u16)(ba->head_sn + ba->buf_size)))
        data_reorder_release_until(ba, ieee80211_sn_inc(ieee80211_sn_sub(sn, ba->buf_size)),
                                   frames);

    index = sn % ba->buf_size;
    if (ba->slot[index])
        goto drop;                      /* already have it */

    /* in order and nothing waiting: straight on */
    if (sn == ba->head_sn && !ba->stored) {
        ba->head_sn = ieee80211_sn_inc(ba->head_sn);
        goto pass;
    }

    ba->slot[index] = skb;
    ba->time[index] = jiffies;
    ba->stored++;
    data_reorder_release(ba, frames);
    return;

pass:
    __skb_queue_tail(frames, skb);
    return;
drop:
    data.stats.rx_dup++;
    data.stats.rx_dropped++;
    kfree_skb(skb);
}

static void data_rx_reorder_timer(struct timer_list *t)
{
    struct data_rx_ba *ba = timer_container_of(ba, t, timer);
    struct sk_buff_head frames;

    __skb_queue_head_init(&frames);
    spin_lock(&data.rx_lock);
    if (ba->active && ba->stored)
        data_reorder_release(ba, &frames);
    data_rx_frames(&frames);
    spin_unlock(&data.rx_lock);
}

/* The AP may now send aggregates on @tid, starting at @ssn, with at most
 * @buf_size frames outstanding. For the MLME. */
int rtw89_data_rx_ba_start(u8 tid, u16 ssn, u16 buf_size)
{
    struct data_rx_ba *ba;

    if (tid >= IEEE80211_NUM_TIDS || !buf_size || buf_size > DATA_REORDER_MAX)
        return -EINVAL;
    rtw89_data_rx_ba_stop(tid);

    ba = &data.rx_ba[tid];
    spin_lock(&data.rx_lock);
    ba->head_sn = ssn & IEEE80211_SN_MASK;
    ba->buf_size = buf_size;
    ba->stored = 0;
    ba->started = false;
    ba->active = true;
    spin_unlock(&data.rx_lock);
    return 0;
}

/* The session is over: frames still waiting are dropped, as in mac80211. */
void rtw89_data_rx_ba_stop(u8 tid)
{
    struct data_rx_ba *ba;
    unsigned int i;

    if (!data.inited || tid >= IEEE80211_NUM_TIDS)
        return;
    ba = &data.rx_ba[tid];

    spin_lock(&data.rx_lock);
    if (ba->active) {
        ba->active = false;
        for (i = 0; i < DATA_REORDER_MAX; i++) {
            kfree_skb(ba->slot[i]);
            ba->slot[i] = NULL;
        }
        ba->stored = 0;
    }
    spin_unlock(&data.rx_lock);
    /* not under the lock: the timer function takes it */
    del_timer_sync(&ba->timer);
}

/* A BlockAck request: the AP has given up on everything before its starting
 * sequence number, so stop waiting for it. Takes the skb (FCS on). */
void rtw89_data_rx_bar(struct sk_buff *skb)
{
    const struct ieee80211_bar *bar = (const void *)skb->data;
    struct sk_buff_head frames;
    struct data_rx_ba *ba;

    __skb_queue_head_init(&frames);
    if (!data.inited || skb->len < sizeof(*bar) + FCS_LEN)
        goto out;

    spin_lock(&data.rx_lock);
    ba = &data.rx_ba[le16_to_cpu(bar->control) >> IEEE80211_BAR_CTRL_TID_INFO_SHIFT];
    if (data.sta && ba->active && ether_addr_equal(bar->ta, data.bssid) &&
        ether_addr_equal(bar->ra, data.addr))
        data_reorder_release_until(ba, le16_to_cpu(bar->start_seq_num) >> 4, &frames);
    data_rx_frames(&frames);
    spin_unlock(&data.rx_lock);
out:
    kfree_skb(skb);
}

/* ------------------------------------------------------------------ */
/*  Receive: entry                                                      */
/* ------------------------------------------------------------------ */

/* A received data frame, FCS still on; takes the skb. Any context that may
 * sleep. */
void rtw89_data_rx(struct sk_buff *skb)
{
    const struct ieee80211_hdr *hdr = (const void *)skb->data;
    __le16 fc = hdr->frame_control;
    struct sk_buff_head frames;
    unsigned int hdrlen, tid;
    u16 sc;

    if (!data.inited || skb->len < sizeof(struct ieee80211_hdr_3addr) + FCS_LEN)
        goto free;
    skb_trim(skb, skb->len - FCS_LEN);

    /* frames with a body, from the AP's side of the network to ours */
    hdrlen = ieee80211_hdrlen(fc);
    if (!ieee80211_is_data_present(fc) || skb->len < hdrlen ||
        !ieee80211_has_fromds(fc) || ieee80211_has_tods(fc))
        goto free;

    __skb_queue_head_init(&frames);
    spin_lock(&data.rx_lock);
    if (!data.sta || !ether_addr_equal(hdr->addr2, data.bssid))
        goto drop;

    if (!is_multicast_ether_addr(hdr->addr1)) {
        if (!ether_addr_equal(hdr->addr1, data.addr))
            goto drop;

        /* ieee80211_rx_h_check_dup(): a retry of the frame we saw last */
        tid = ieee80211_is_data_qos(fc) ?
              ieee80211_get_qos_ctl((struct ieee80211_hdr *)hdr)[0] & IEEE80211_QOS_CTL_TID_MASK :
              DATA_NOQOS;
        sc = le16_to_cpu(hdr->seq_ctrl);
        if (ieee80211_has_retry(fc) && data.rx_sc_valid[tid] && data.rx_last_sc[tid] == sc) {
            data.stats.rx_dup++;
            goto drop;
        }
        data.rx_last_sc[tid] = sc;
        data.rx_sc_valid[tid] = true;
    }

    data_rx_reorder(skb, &frames);
    data_rx_frames(&frames);
    spin_unlock(&data.rx_lock);
    return;

drop:
    data.stats.rx_dropped++;
    spin_unlock(&data.rx_lock);
free:
    kfree_skb(skb);
}
