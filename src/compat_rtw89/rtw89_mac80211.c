// SPDX-License-Identifier: GPL-2.0
/*
 * The part of Linux mac80211 (net/mac80211) that rtw89 calls, see
 * rtw89_net80211.h.
 *
 * What is real here: hw allocation, work queueing, the vif/station/key lists
 * behind the iterators, the per-TXQ frame queues and their scheduling, queue
 * stop/wake, the null-func / PS-Poll / probe-request templates the firmware
 * needs, and the emulated chanctx ops. What is forwarded to the IOKit layer:
 * received frames, TX status, scan completion, link events, BlockAck session
 * requests. What is not implemented yet is listed at the end of the file.
 */
#include "rtw89_net80211.h"

/* ------------------------------------------------------------------ */
/*  hw allocation / registration                                        */
/* ------------------------------------------------------------------ */

static void rtw89_m80211_wake_txqs_work(struct work_struct *work);

struct ieee80211_hw *ieee80211_alloc_hw_nm(size_t priv_data_len,
                                           const struct ieee80211_ops *ops,
                                           const char *requested_name)
{
    struct rtw89_m80211_local *local;
    struct wiphy *wiphy;
    size_t priv_size;
    int ac;

    if (WARN_ON(!ops->tx || !ops->start || !ops->stop || !ops->config ||
                !ops->add_interface || !ops->remove_interface ||
                !ops->configure_filter || !ops->wake_tx_queue))
        return NULL;

    /* Like mac80211: driver private data sits behind the local, aligned. */
    priv_size = ALIGN(sizeof(*local), NETDEV_ALIGN) + priv_data_len;

    wiphy = wiphy_new_nm(NULL, (int)priv_size, requested_name);
    if (!wiphy)
        return NULL;

    local = wiphy_priv(wiphy);
    local->hw.wiphy = wiphy;
    local->hw.priv = (char *)local + ALIGN(sizeof(*local), NETDEV_ALIGN);
    local->ops = ops;

    /* mac80211 defaults a driver may rely on without setting them. */
    local->hw.queues = 1;
    local->hw.max_rates = 1;
    local->hw.max_report_rates = 0;
    local->hw.max_rx_aggregation_subframes = IEEE80211_MAX_AMPDU_BUF_HT;
    local->hw.max_tx_aggregation_subframes = IEEE80211_MAX_AMPDU_BUF_HT;
    local->hw.offchannel_tx_hw_queue = IEEE80211_INVAL_HW_QUEUE;
    local->hw.conf.long_frame_max_tx_count = 4;
    local->hw.conf.short_frame_max_tx_count = 7;
    local->hw.radiotap_mcs_details = IEEE80211_RADIOTAP_MCS_HAVE_MCS |
                                     IEEE80211_RADIOTAP_MCS_HAVE_GI |
                                     IEEE80211_RADIOTAP_MCS_HAVE_BW;
    local->hw.radiotap_vht_details = IEEE80211_RADIOTAP_VHT_KNOWN_GI |
                                     IEEE80211_RADIOTAP_VHT_KNOWN_BANDWIDTH;
    local->hw.max_mtu = IEEE80211_MAX_DATA_LEN;

    spin_lock_init(&local->lock);
    INIT_LIST_HEAD(&local->vifs);
    INIT_LIST_HEAD(&local->stas);
    INIT_LIST_HEAD(&local->keys);
    for (ac = 0; ac < IEEE80211_NUM_ACS; ac++) {
        spin_lock_init(&local->active_txq_lock[ac]);
        INIT_LIST_HEAD(&local->active_txqs[ac]);
    }
    INIT_WORK(&local->wake_txqs_work, rtw89_m80211_wake_txqs_work);

    return &local->hw;
}

void ieee80211_free_hw(struct ieee80211_hw *hw)
{
    struct rtw89_m80211_local *local = hw_to_local(hw);
    int ac;

    cancel_work_sync(&local->wake_txqs_work);
    WARN_ON(!list_empty(&local->vifs));
    WARN_ON(!list_empty(&local->stas));
    for (ac = 0; ac < IEEE80211_NUM_ACS; ac++)
        spin_lock_destroy(&local->active_txq_lock[ac]);
    spin_lock_destroy(&local->lock);
    wiphy_free(hw->wiphy);
}

struct ieee80211_hw *wiphy_to_ieee80211_hw(struct wiphy *wiphy)
{
    struct rtw89_m80211_local *local = wiphy_priv(wiphy);

    return &local->hw;
}

int ieee80211_register_hw(struct ieee80211_hw *hw)
{
    struct rtw89_m80211_local *local = hw_to_local(hw);
    struct ieee80211_supported_band *sband;
    int band;

    /* First usable channel, for the emulated chanctx ops (as mac80211 does). */
    for (band = 0; band < NUM_NL80211_BANDS; band++) {
        sband = hw->wiphy->bands[band];
        if (!sband || !sband->n_channels)
            continue;
        if (!local->dflt_chandef.chan) {
            cfg80211_chandef_create(&local->dflt_chandef, &sband->channels[0],
                                    NL80211_CHAN_NO_HT);
            hw->conf.chandef = local->dflt_chandef;
        }
    }
    if (!local->dflt_chandef.chan)
        return -EINVAL;

    local->registered = true;
    return 0;
}

void ieee80211_unregister_hw(struct ieee80211_hw *hw)
{
    struct rtw89_m80211_local *local = hw_to_local(hw);

    local->registered = false;
    cancel_work_sync(&local->wake_txqs_work);
}

void rtw89_m80211_set_glue(struct ieee80211_hw *hw,
                           const struct rtw89_m80211_glue_ops *ops, void *ctx)
{
    struct rtw89_m80211_local *local = hw_to_local(hw);

    local->glue_ctx = ctx;
    local->glue = ops;
}

/* ------------------------------------------------------------------ */
/*  Work                                                                */
/* ------------------------------------------------------------------ */

void ieee80211_queue_work(struct ieee80211_hw *hw, struct work_struct *work)
{
    schedule_work(work);
}

void ieee80211_queue_delayed_work(struct ieee80211_hw *hw,
                                  struct delayed_work *dwork, unsigned long delay)
{
    schedule_delayed_work(dwork, delay);
}

/* ------------------------------------------------------------------ */
/*  TX queues                                                           */
/* ------------------------------------------------------------------ */

static struct ieee80211_txq *rtw89_m80211_txq_alloc(struct rtw89_m80211_local *local,
                                                    struct ieee80211_vif *vif,
                                                    struct ieee80211_sta *sta, u8 tid)
{
    /* 802.1D priority (== TID & 7) to access category, as in mac80211. */
    static const u8 tid_to_ac[8] = {
        IEEE80211_AC_BE, IEEE80211_AC_BK, IEEE80211_AC_BK, IEEE80211_AC_BE,
        IEEE80211_AC_VI, IEEE80211_AC_VI, IEEE80211_AC_VO, IEEE80211_AC_VO,
    };
    struct rtw89_m80211_txq *mtxq;

    mtxq = kzalloc(sizeof(*mtxq) + local->hw.txq_data_size, GFP_KERNEL);
    if (!mtxq)
        return NULL;

    INIT_LIST_HEAD(&mtxq->schedule_entry);
    skb_queue_head_init(&mtxq->frames);
    mtxq->txq.vif = vif;
    mtxq->txq.sta = sta;
    if (!sta) {
        /* per-vif queue: broadcast/multicast and frames without a station */
        mtxq->txq.tid = 0;
        mtxq->txq.ac = IEEE80211_AC_BE;
    } else if (tid == IEEE80211_NUM_TIDS) {
        /* management frames to this station */
        mtxq->txq.tid = tid;
        mtxq->txq.ac = IEEE80211_AC_VO;
    } else {
        mtxq->txq.tid = tid;
        mtxq->txq.ac = tid_to_ac[tid & 7];
    }
    return &mtxq->txq;
}

static void rtw89_m80211_txq_free(struct rtw89_m80211_local *local,
                                  struct ieee80211_txq *txq)
{
    struct rtw89_m80211_txq *mtxq;
    struct sk_buff *skb;

    if (!txq)
        return;
    mtxq = to_mtxq(txq);

    spin_lock_bh(&local->active_txq_lock[txq->ac]);
    if (!list_empty(&mtxq->schedule_entry))
        list_del_init(&mtxq->schedule_entry);
    spin_unlock_bh(&local->active_txq_lock[txq->ac]);

    while ((skb = skb_dequeue(&mtxq->frames)))
        ieee80211_free_txskb(&local->hw, skb);
    spin_lock_destroy(&mtxq->frames.lock);
    kfree(mtxq);
}

void rtw89_m80211_tx(struct ieee80211_hw *hw, struct ieee80211_txq *txq,
                     struct sk_buff *skb)
{
    struct rtw89_m80211_local *local = hw_to_local(hw);
    struct rtw89_m80211_txq *mtxq = to_mtxq(txq);

    spin_lock_bh(&mtxq->frames.lock);
    __skb_queue_tail(&mtxq->frames, skb);
    mtxq->byte_cnt += skb->len;
    spin_unlock_bh(&mtxq->frames.lock);

    if (!local->queues_stopped)
        local->ops->wake_tx_queue(hw, txq);
}

struct sk_buff *ieee80211_tx_dequeue(struct ieee80211_hw *hw,
                                     struct ieee80211_txq *txq)
{
    struct rtw89_m80211_local *local = hw_to_local(hw);
    struct rtw89_m80211_txq *mtxq = to_mtxq(txq);
    struct sk_buff *skb = NULL;

    if (local->queues_stopped)
        return NULL;

    spin_lock_bh(&mtxq->frames.lock);
    if (!skb_queue_empty(&mtxq->frames)) {
        skb = skb_peek(&mtxq->frames);
        __skb_unlink(skb, &mtxq->frames);
        mtxq->byte_cnt -= skb->len;
    }
    spin_unlock_bh(&mtxq->frames.lock);

    return skb;
}

void ieee80211_txq_get_depth(struct ieee80211_txq *txq,
                             unsigned long *frame_cnt, unsigned long *byte_cnt)
{
    struct rtw89_m80211_txq *mtxq = to_mtxq(txq);

    if (frame_cnt)
        *frame_cnt = skb_queue_len(&mtxq->frames);
    if (byte_cnt)
        *byte_cnt = mtxq->byte_cnt;
}

/*
 * Scheduling, as mac80211 without airtime fairness: one FIFO of TXQs per AC.
 * A round starts with ieee80211_txq_schedule_start(); ieee80211_next_txq()
 * hands out each scheduled TXQ at most once per round and the driver puts it
 * back with ieee80211_return_txq() if it still has frames.
 */
void ieee80211_txq_schedule_start(struct ieee80211_hw *hw, u8 ac)
{
    struct rtw89_m80211_local *local = hw_to_local(hw);

    spin_lock_bh(&local->active_txq_lock[ac]);
    local->schedule_round[ac]++;
    /* 0 means "never served"; skip it when the counter wraps. */
    if (!local->schedule_round[ac])
        local->schedule_round[ac] = 1;
    spin_unlock_bh(&local->active_txq_lock[ac]);
}

struct ieee80211_txq *ieee80211_next_txq(struct ieee80211_hw *hw, u8 ac)
{
    struct rtw89_m80211_local *local = hw_to_local(hw);
    struct ieee80211_txq *ret = NULL;
    struct rtw89_m80211_txq *mtxq;

    spin_lock_bh(&local->active_txq_lock[ac]);
    mtxq = list_first_entry_or_null(&local->active_txqs[ac],
                                    struct rtw89_m80211_txq, schedule_entry);
    if (mtxq && mtxq->schedule_round != local->schedule_round[ac]) {
        list_del_init(&mtxq->schedule_entry);
        mtxq->schedule_round = local->schedule_round[ac];
        ret = &mtxq->txq;
    }
    spin_unlock_bh(&local->active_txq_lock[ac]);

    return ret;
}

void __ieee80211_schedule_txq(struct ieee80211_hw *hw, struct ieee80211_txq *txq,
                              bool force)
{
    struct rtw89_m80211_local *local = hw_to_local(hw);
    struct rtw89_m80211_txq *mtxq = to_mtxq(txq);

    spin_lock_bh(&local->active_txq_lock[txq->ac]);
    if (list_empty(&mtxq->schedule_entry) &&
        (force || !skb_queue_empty(&mtxq->frames)))
        list_add_tail(&mtxq->schedule_entry, &local->active_txqs[txq->ac]);
    spin_unlock_bh(&local->active_txq_lock[txq->ac]);
}

/* ------------------------------------------------------------------ */
/*  Queue stop / wake                                                   */
/* ------------------------------------------------------------------ */

static void rtw89_m80211_wake_txq(struct rtw89_m80211_local *local,
                                  struct ieee80211_txq *txq)
{
    if (txq && !skb_queue_empty(&to_mtxq(txq)->frames))
        local->ops->wake_tx_queue(&local->hw, txq);
}

/* The driver wakes queues from inside its own locks; restart TX from a work. */
static void rtw89_m80211_wake_txqs_work(struct work_struct *work)
{
    struct rtw89_m80211_local *local =
        container_of(work, struct rtw89_m80211_local, wake_txqs_work);
    struct rtw89_m80211_vif *mvif;
    struct rtw89_m80211_sta *msta;
    int i;

    spin_lock_bh(&local->lock);
    if (local->queues_stopped || !local->registered)
        goto out;

    list_for_each_entry(msta, &local->stas, list)
        for (i = 0; i < ARRAY_SIZE(msta->sta.txq); i++)
            rtw89_m80211_wake_txq(local, msta->sta.txq[i]);
    list_for_each_entry(mvif, &local->vifs, list)
        rtw89_m80211_wake_txq(local, mvif->vif.txq);
out:
    spin_unlock_bh(&local->lock);
}

void ieee80211_stop_queues(struct ieee80211_hw *hw)
{
    struct rtw89_m80211_local *local = hw_to_local(hw);

    local->queues_stopped = true;
    if (local->glue && local->glue->queues_stopped)
        local->glue->queues_stopped(local->glue_ctx, true);
}

void ieee80211_wake_queues(struct ieee80211_hw *hw)
{
    struct rtw89_m80211_local *local = hw_to_local(hw);

    local->queues_stopped = false;
    if (local->glue && local->glue->queues_stopped)
        local->glue->queues_stopped(local->glue_ctx, false);
    schedule_work(&local->wake_txqs_work);
}

/* ------------------------------------------------------------------ */
/*  RX / TX status / scan: forwarded to the IOKit layer                 */
/* ------------------------------------------------------------------ */

void ieee80211_rx_napi(struct ieee80211_hw *hw, struct ieee80211_sta *sta,
                       struct sk_buff *skb, struct napi_struct *napi)
{
    struct rtw89_m80211_local *local = hw_to_local(hw);

    if (local->glue && local->glue->rx)
        local->glue->rx(local->glue_ctx, sta, skb);
    else
        kfree_skb(skb);
}

void ieee80211_tx_status_skb(struct ieee80211_hw *hw, struct sk_buff *skb)
{
    struct rtw89_m80211_local *local = hw_to_local(hw);

    if (local->glue && local->glue->tx_status)
        local->glue->tx_status(local->glue_ctx, skb);
    else
        kfree_skb(skb);
}

void ieee80211_tx_status_irqsafe(struct ieee80211_hw *hw, struct sk_buff *skb)
{
    /* No hard-IRQ context here: the "interrupt" is already a thread. */
    ieee80211_tx_status_skb(hw, skb);
}

void ieee80211_free_txskb(struct ieee80211_hw *hw, struct sk_buff *skb)
{
    kfree_skb(skb);
}

void ieee80211_scan_completed(struct ieee80211_hw *hw,
                              struct cfg80211_scan_info *info)
{
    struct rtw89_m80211_local *local = hw_to_local(hw);

    if (local->glue && local->glue->scan_done)
        local->glue->scan_done(local->glue_ctx, info->aborted);
}

static void rtw89_m80211_link_event(struct ieee80211_vif *vif,
                                    enum rtw89_m80211_link_event event, s32 rssi)
{
    struct rtw89_m80211_local *local = to_mvif(vif)->local;

    if (local->glue && local->glue->link_event)
        local->glue->link_event(local->glue_ctx, vif, event, rssi);
}

void ieee80211_connection_loss(struct ieee80211_vif *vif)
{
    rtw89_m80211_link_event(vif, RTW89_M80211_CONNECTION_LOSS, 0);
}

void ieee80211_beacon_loss(struct ieee80211_vif *vif)
{
    rtw89_m80211_link_event(vif, RTW89_M80211_BEACON_LOSS, 0);
}

void ieee80211_cqm_rssi_notify(struct ieee80211_vif *vif,
                               enum nl80211_cqm_rssi_threshold_event rssi_event,
                               s32 rssi_level, gfp_t gfp)
{
    rtw89_m80211_link_event(vif,
                            rssi_event == NL80211_CQM_RSSI_THRESHOLD_EVENT_LOW ?
                            RTW89_M80211_CQM_RSSI_LOW : RTW89_M80211_CQM_RSSI_HIGH,
                            rssi_level);
}

/* ------------------------------------------------------------------ */
/*  Interfaces, stations, keys                                          */
/* ------------------------------------------------------------------ */

struct ieee80211_vif *rtw89_m80211_vif_alloc(struct ieee80211_hw *hw,
                                             enum nl80211_iftype type, const u8 *addr)
{
    struct rtw89_m80211_local *local = hw_to_local(hw);
    struct rtw89_m80211_vif *mvif;

    mvif = kzalloc(sizeof(*mvif) + hw->vif_data_size, GFP_KERNEL);
    if (!mvif)
        return NULL;

    mvif->local = local;
    mvif->vif.type = type;
    ether_addr_copy(mvif->vif.addr, addr);
    mvif->vif.addr_valid = true;
    /* Not an MLD: link 0 is the vif's own bss_conf (the "deflink"). */
    mvif->vif.bss_conf.vif = &mvif->vif;
    mvif->vif.bss_conf.link_id = 0;
    ether_addr_copy(mvif->vif.bss_conf.addr, addr);
    mvif->vif.link_conf[0] = &mvif->vif.bss_conf;

    mvif->vif.txq = rtw89_m80211_txq_alloc(local, &mvif->vif, NULL, 0);
    if (!mvif->vif.txq) {
        kfree(mvif);
        return NULL;
    }

    spin_lock_bh(&local->lock);
    list_add_tail(&mvif->list, &local->vifs);
    spin_unlock_bh(&local->lock);

    return &mvif->vif;
}

void rtw89_m80211_vif_free(struct ieee80211_vif *vif)
{
    struct rtw89_m80211_vif *mvif = to_mvif(vif);
    struct rtw89_m80211_local *local = mvif->local;

    spin_lock_bh(&local->lock);
    list_del(&mvif->list);
    spin_unlock_bh(&local->lock);

    rtw89_m80211_txq_free(local, vif->txq);
    kfree(mvif);
}

void rtw89_m80211_vif_set_in_driver(struct ieee80211_vif *vif, bool in_driver)
{
    to_mvif(vif)->in_driver = in_driver;
}

void ieee80211_iterate_active_interfaces_atomic(
    struct ieee80211_hw *hw, u32 iter_flags,
    void (*iterator)(void *data, u8 *mac, struct ieee80211_vif *vif),
    void *data)
{
    struct rtw89_m80211_local *local = hw_to_local(hw);
    struct rtw89_m80211_vif *mvif;

    spin_lock_bh(&local->lock);
    list_for_each_entry(mvif, &local->vifs, list)
        if (mvif->in_driver)
            iterator(data, mvif->vif.addr, &mvif->vif);
    spin_unlock_bh(&local->lock);
}

struct ieee80211_sta *rtw89_m80211_sta_alloc(struct ieee80211_vif *vif, const u8 *addr)
{
    struct rtw89_m80211_vif *mvif = to_mvif(vif);
    struct rtw89_m80211_local *local = mvif->local;
    struct rtw89_m80211_sta *msta;
    int i;

    msta = kzalloc(sizeof(*msta) + local->hw.sta_data_size, GFP_KERNEL);
    if (!msta)
        return NULL;

    msta->mvif = mvif;
    ether_addr_copy(msta->sta.addr, addr);
    /* Not an MLD: link 0 is the station's deflink. */
    msta->sta.deflink.sta = &msta->sta;
    msta->sta.deflink.link_id = 0;
    ether_addr_copy(msta->sta.deflink.addr, addr);
    msta->sta.link[0] = &msta->sta.deflink;
    msta->sta.cur = &msta->sta.deflink.agg;
    msta->sta.max_rx_aggregation_subframes = local->hw.max_rx_aggregation_subframes;

    for (i = 0; i < ARRAY_SIZE(msta->sta.txq); i++) {
        /* The management-frame TXQ is opt-in; without it those frames go
         * straight to ops->tx(). */
        if (i == IEEE80211_NUM_TIDS &&
            !(vif->type == NL80211_IFTYPE_STATION ?
              ieee80211_hw_check(&local->hw, STA_MMPDU_TXQ) :
              ieee80211_hw_check(&local->hw, BUFF_MMPDU_TXQ)))
            continue;
        msta->sta.txq[i] = rtw89_m80211_txq_alloc(local, vif, &msta->sta, (u8)i);
        if (!msta->sta.txq[i])
            goto err;
    }

    spin_lock_bh(&local->lock);
    list_add_tail(&msta->list, &local->stas);
    spin_unlock_bh(&local->lock);

    return &msta->sta;

err:
    while (--i >= 0)
        rtw89_m80211_txq_free(local, msta->sta.txq[i]);
    kfree(msta);
    return NULL;
}

void rtw89_m80211_sta_free(struct ieee80211_sta *sta)
{
    struct rtw89_m80211_sta *msta = to_msta(sta);
    struct rtw89_m80211_local *local = msta->mvif->local;
    int i;

    spin_lock_bh(&local->lock);
    list_del(&msta->list);
    spin_unlock_bh(&local->lock);

    for (i = 0; i < ARRAY_SIZE(sta->txq); i++)
        rtw89_m80211_txq_free(local, sta->txq[i]);
    kfree(msta);
}

void rtw89_m80211_sta_set_uploaded(struct ieee80211_sta *sta, bool uploaded)
{
    to_msta(sta)->uploaded = uploaded;
}

/* Next uploaded station after @prev (NULL starts over); see for_each_station(). */
struct ieee80211_sta *__ieee80211_iterate_stations(struct ieee80211_hw *hw,
                                                   struct ieee80211_sta *prev)
{
    struct rtw89_m80211_local *local = hw_to_local(hw);
    struct ieee80211_sta *ret = NULL;
    struct rtw89_m80211_sta *msta;
    bool found = !prev;

    spin_lock_bh(&local->lock);
    list_for_each_entry(msta, &local->stas, list) {
        if (!found) {
            found = &msta->sta == prev;
            continue;
        }
        if (msta->uploaded) {
            ret = &msta->sta;
            break;
        }
    }
    spin_unlock_bh(&local->lock);

    return ret;
}

void ieee80211_iterate_stations_atomic(
    struct ieee80211_hw *hw,
    void (*iterator)(void *data, struct ieee80211_sta *sta),
    void *data)
{
    struct rtw89_m80211_local *local = hw_to_local(hw);
    struct rtw89_m80211_sta *msta;

    spin_lock_bh(&local->lock);
    list_for_each_entry(msta, &local->stas, list)
        if (msta->uploaded)
            iterator(data, &msta->sta);
    spin_unlock_bh(&local->lock);
}

struct ieee80211_sta *ieee80211_find_sta(struct ieee80211_vif *vif, const u8 *addr)
{
    struct rtw89_m80211_vif *mvif;
    struct rtw89_m80211_local *local;
    struct ieee80211_sta *ret = NULL;
    struct rtw89_m80211_sta *msta;

    if (!vif)
        return NULL;
    mvif = to_mvif(vif);
    local = mvif->local;

    spin_lock_bh(&local->lock);
    list_for_each_entry(msta, &local->stas, list) {
        if (msta->mvif == mvif && msta->uploaded &&
            ether_addr_equal(msta->sta.addr, addr)) {
            ret = &msta->sta;
            break;
        }
    }
    spin_unlock_bh(&local->lock);

    return ret;
}

void ieee80211_sta_recalc_aggregates(struct ieee80211_sta *pubsta)
{
    /* Only MLO stations aggregate across links. */
    pubsta->cur = &pubsta->deflink.agg;
}

int rtw89_m80211_key_add(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                         struct ieee80211_sta *sta, struct ieee80211_key_conf *conf)
{
    struct rtw89_m80211_local *local = hw_to_local(hw);
    struct rtw89_m80211_key *key;

    key = kzalloc(sizeof(*key), GFP_KERNEL);
    if (!key)
        return -ENOMEM;
    key->vif = vif;
    key->sta = sta;
    key->conf = conf;

    spin_lock_bh(&local->lock);
    list_add_tail(&key->list, &local->keys);
    spin_unlock_bh(&local->lock);
    return 0;
}

void rtw89_m80211_key_del(struct ieee80211_hw *hw, struct ieee80211_key_conf *conf)
{
    struct rtw89_m80211_local *local = hw_to_local(hw);
    struct rtw89_m80211_key *key, *tmp;

    spin_lock_bh(&local->lock);
    list_for_each_entry_safe(key, tmp, &local->keys, list) {
        if (key->conf == conf) {
            list_del(&key->list);
            kfree(key);
        }
    }
    spin_unlock_bh(&local->lock);
}

void ieee80211_iter_keys_rcu(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                             void (*iter)(struct ieee80211_hw *hw,
                                          struct ieee80211_vif *vif,
                                          struct ieee80211_sta *sta,
                                          struct ieee80211_key_conf *key,
                                          void *data),
                             void *iter_data)
{
    struct rtw89_m80211_local *local = hw_to_local(hw);
    struct rtw89_m80211_key *key;

    spin_lock_bh(&local->lock);
    list_for_each_entry(key, &local->keys, list)
        if (!vif || key->vif == vif)
            iter(hw, key->vif, key->sta, key->conf, iter_data);
    spin_unlock_bh(&local->lock);
}

/* ------------------------------------------------------------------ */
/*  Frame templates (the firmware sends these on its own)               */
/* ------------------------------------------------------------------ */

struct sk_buff *ieee80211_nullfunc_get(struct ieee80211_hw *hw,
                                       struct ieee80211_vif *vif,
                                       int link_id, bool qos_ok)
{
    struct ieee80211_hdr_3addr *nullfunc;
    struct sk_buff *skb;
    bool qos = false;

    if (WARN_ON(vif->type != NL80211_IFTYPE_STATION))
        return NULL;
    /* Not an MLD: only the default link exists. */
    if (link_id > 0)
        return NULL;

    skb = dev_alloc_skb(hw->extra_tx_headroom + sizeof(*nullfunc) + 2);
    if (!skb)
        return NULL;

    if (qos_ok) {
        struct ieee80211_sta *sta = ieee80211_find_sta(vif, vif->cfg.ap_addr);

        qos = sta && sta->wme;
    }

    skb_reserve(skb, hw->extra_tx_headroom);

    nullfunc = skb_put_zero(skb, sizeof(*nullfunc));
    nullfunc->frame_control = cpu_to_le16(IEEE80211_FTYPE_DATA |
                                          IEEE80211_STYPE_NULLFUNC |
                                          IEEE80211_FCTL_TODS);
    if (qos) {
        __le16 qoshdr = cpu_to_le16(7);

        nullfunc->frame_control |= cpu_to_le16(IEEE80211_STYPE_QOS_NULLFUNC);
        skb->priority = 7;
        skb_set_queue_mapping(skb, IEEE80211_AC_VO);
        skb_put_data(skb, &qoshdr, sizeof(qoshdr));
    }

    memcpy(nullfunc->addr1, vif->cfg.ap_addr, ETH_ALEN);
    memcpy(nullfunc->addr2, vif->addr, ETH_ALEN);
    memcpy(nullfunc->addr3, vif->cfg.ap_addr, ETH_ALEN);

    return skb;
}

struct sk_buff *ieee80211_pspoll_get(struct ieee80211_hw *hw,
                                     struct ieee80211_vif *vif)
{
    struct ieee80211_pspoll *pspoll;
    struct sk_buff *skb;

    if (WARN_ON(vif->type != NL80211_IFTYPE_STATION))
        return NULL;

    skb = dev_alloc_skb(hw->extra_tx_headroom + sizeof(*pspoll));
    if (!skb)
        return NULL;

    skb_reserve(skb, hw->extra_tx_headroom);

    pspoll = skb_put_zero(skb, sizeof(*pspoll));
    pspoll->frame_control = cpu_to_le16(IEEE80211_FTYPE_CTL |
                                        IEEE80211_STYPE_PSPOLL);
    /* The AID in a PS-Poll has its two MSBs set. */
    pspoll->aid = cpu_to_le16(vif->cfg.aid | 1 << 15 | 1 << 14);
    memcpy(pspoll->bssid, vif->cfg.ap_addr, ETH_ALEN);
    memcpy(pspoll->ta, vif->addr, ETH_ALEN);

    return skb;
}

struct sk_buff *ieee80211_probereq_get(struct ieee80211_hw *hw, const u8 *src_addr,
                                       const u8 *ssid, size_t ssid_len,
                                       size_t tailroom)
{
    struct ieee80211_hdr_3addr *hdr;
    size_t ie_ssid_len = 2 + ssid_len;
    struct sk_buff *skb;
    u8 *pos;

    skb = dev_alloc_skb(hw->extra_tx_headroom + sizeof(*hdr) + ie_ssid_len +
                        tailroom);
    if (!skb)
        return NULL;

    skb_reserve(skb, hw->extra_tx_headroom);

    hdr = skb_put_zero(skb, sizeof(*hdr));
    hdr->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
                                     IEEE80211_STYPE_PROBE_REQ);
    eth_broadcast_addr(hdr->addr1);
    memcpy(hdr->addr2, src_addr, ETH_ALEN);
    eth_broadcast_addr(hdr->addr3);

    pos = skb_put(skb, ie_ssid_len);
    *pos++ = WLAN_EID_SSID;
    *pos++ = (u8)ssid_len;
    if (ssid_len)
        memcpy(pos, ssid, ssid_len);

    return skb;
}

/* ------------------------------------------------------------------ */
/*  Emulated channel contexts (mac80211/main.c)                         */
/* ------------------------------------------------------------------ */

/* rtw89 installs these when the firmware cannot do real channel contexts:
 * the one context's chandef becomes hw->conf.chandef and ->config() is told. */
static void rtw89_m80211_hw_conf_chan(struct rtw89_m80211_local *local,
                                      struct ieee80211_chanctx_conf *ctx)
{
    struct cfg80211_chan_def chandef = local->dflt_chandef;

    if (ctx && !WARN_ON(!ctx->def.chan))
        chandef = ctx->def;

    if (cfg80211_chandef_identical(&chandef, &local->hw.conf.chandef))
        return;

    local->hw.conf.chandef = chandef;
    local->ops->config(&local->hw, -1, IEEE80211_CONF_CHANGE_CHANNEL);
}

int ieee80211_emulate_add_chanctx(struct ieee80211_hw *hw,
                                  struct ieee80211_chanctx_conf *ctx)
{
    struct rtw89_m80211_local *local = hw_to_local(hw);

    hw->conf.radar_enabled = ctx->radar_enabled;
    rtw89_m80211_hw_conf_chan(local, ctx);
    return 0;
}

void ieee80211_emulate_remove_chanctx(struct ieee80211_hw *hw,
                                      struct ieee80211_chanctx_conf *ctx)
{
    struct rtw89_m80211_local *local = hw_to_local(hw);

    hw->conf.radar_enabled = false;
    rtw89_m80211_hw_conf_chan(local, NULL);
}

void ieee80211_emulate_change_chanctx(struct ieee80211_hw *hw,
                                      struct ieee80211_chanctx_conf *ctx,
                                      u32 changed)
{
    struct rtw89_m80211_local *local = hw_to_local(hw);

    hw->conf.radar_enabled = ctx->radar_enabled;
    rtw89_m80211_hw_conf_chan(local, ctx);
}

int ieee80211_emulate_switch_vif_chanctx(struct ieee80211_hw *hw,
                                         struct ieee80211_vif_chanctx_switch *vifs,
                                         int n_vifs,
                                         enum ieee80211_chanctx_switch_mode mode)
{
    struct rtw89_m80211_local *local = hw_to_local(hw);

    if (n_vifs <= 0)
        return -EINVAL;

    hw->conf.radar_enabled = vifs[0].new_ctx->radar_enabled;
    rtw89_m80211_hw_conf_chan(local, vifs[0].new_ctx);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  BlockAck sessions: negotiated by the IOKit layer's MLME             */
/* ------------------------------------------------------------------ */

int ieee80211_start_tx_ba_session(struct ieee80211_sta *sta, u16 tid, u16 timeout)
{
    struct rtw89_m80211_local *local = to_msta(sta)->mvif->local;

    if (local->glue && local->glue->start_tx_ba)
        return local->glue->start_tx_ba(local->glue_ctx, sta, tid, timeout);
    return -EOPNOTSUPP;
}

int ieee80211_stop_tx_ba_session(struct ieee80211_sta *sta, u16 tid)
{
    struct rtw89_m80211_local *local = to_msta(sta)->mvif->local;

    if (local->glue && local->glue->stop_tx_ba)
        return local->glue->stop_tx_ba(local->glue_ctx, sta, tid);
    return -EOPNOTSUPP;
}

void ieee80211_stop_tx_ba_cb_irqsafe(struct ieee80211_vif *vif, const u8 *ra, u16 tid)
{
    /* The driver confirms it has stopped aggregating; nothing is waiting on it
     * until the IOKit layer negotiates sessions. */
}

/* ------------------------------------------------------------------ */
/*  Not implemented yet                                                 */
/* ------------------------------------------------------------------ */

/*
 * Firmware error recovery (SER level 2) asks mac80211 to stop the device and
 * replay its whole configuration. That replay lives in the IOKit layer; until
 * it exists the request is only reported. (Milestone M4.)
 */
void ieee80211_restart_hw(struct ieee80211_hw *hw)
{
    struct rtw89_m80211_local *local = hw_to_local(hw);

    if (local->glue && local->glue->restart) {
        local->glue->restart(local->glue_ctx);
        return;
    }
    pr_err("rtw89: firmware asked for a hardware restart, which is not implemented\n");
}

/* MLO: the RTL8852B is a single-link chip, so no vif is an MLD. */
int ieee80211_set_active_links(struct ieee80211_vif *vif, u16 active_links)
{
    return -EINVAL;
}

/* AP mode (beacons, probe responses, channel-switch countdown, client power
 * save) and remain-on-channel (P2P) are out of scope for the station port. */
struct sk_buff *ieee80211_beacon_get_tim(struct ieee80211_hw *hw,
                                         struct ieee80211_vif *vif,
                                         u16 *tim_offset, u16 *tim_length,
                                         unsigned int link_id)
{
    return NULL;
}

struct sk_buff *ieee80211_proberesp_get(struct ieee80211_hw *hw,
                                        struct ieee80211_vif *vif)
{
    return NULL;
}

bool ieee80211_beacon_cntdwn_is_complete(struct ieee80211_vif *vif,
                                         unsigned int link_id)
{
    return false;
}

void ieee80211_csa_finish(struct ieee80211_vif *vif, unsigned int link_id)
{
}

int ieee80211_sta_ps_transition(struct ieee80211_sta *sta, bool start)
{
    return -EINVAL;
}

void ieee80211_sta_pspoll(struct ieee80211_sta *sta)
{
}

void ieee80211_sta_uapsd_trigger(struct ieee80211_sta *sta, u8 tid)
{
}

void ieee80211_ready_on_channel(struct ieee80211_hw *hw)
{
}

void ieee80211_remain_on_channel_expired(struct ieee80211_hw *hw)
{
}
