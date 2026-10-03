// SPDX-License-Identifier: GPL-2.0
/*
 * The part of Linux cfg80211 (net/wireless) that rtw89 calls, see
 * rtw89_net80211.h. Behaviour follows net/wireless/{core,util,chan,scan,reg}.c.
 */
#include "rtw89_net80211.h"

/* ------------------------------------------------------------------ */
/*  wiphy allocation                                                    */
/* ------------------------------------------------------------------ */

static void rtw89_cfg80211_wiphy_work(struct work_struct *work);
static void rtw89_cfg80211_reg_work(struct work_struct *work);

struct wiphy *wiphy_new_nm(const struct cfg80211_ops *ops, int sizeof_priv,
                           const char *requested_name)
{
    struct rtw89_cfg80211_rdev *rdev;
    size_t size = sizeof(*rdev) + sizeof_priv;
    void *base;

    /* kmalloc only guarantees pointer alignment; struct wiphy asks for more. */
    base = kzalloc(size + NETDEV_ALIGN, GFP_KERNEL);
    if (!base)
        return NULL;

    rdev = (struct rtw89_cfg80211_rdev *)ALIGN((uintptr_t)base, (uintptr_t)NETDEV_ALIGN);
    rdev->alloc_base = base;
    rdev->ops = ops;

    /*
     * Own thread: the runner sleeps on the wiphy mutex, and the inherited
     * workqueues are single-threaded, so on system_wq it would stall every
     * other work item behind it, including ones the mutex holder waits for.
     */
    rdev->wq = alloc_ordered_workqueue("rtw89_wiphy", 0);
    if (!rdev->wq) {
        kfree(base);
        return NULL;
    }

    spin_lock_init(&rdev->wiphy_work_lock);
    INIT_LIST_HEAD(&rdev->wiphy_work_list);
    INIT_WORK(&rdev->wiphy_work, rtw89_cfg80211_wiphy_work);
    INIT_WORK(&rdev->reg_work, rtw89_cfg80211_reg_work);

    mutex_init(&rdev->wiphy.mtx);
    rdev->wiphy.dev.name = requested_name ? requested_name : "phy0";

    /* The defaults of net/wireless/core.c. (u32)-1 means "off": rtw89 programs
     * the RTS threshold into the chip, and zero would put an RTS/CTS exchange
     * in front of nearly every frame. */
    rdev->wiphy.retry_short = 7;
    rdev->wiphy.retry_long = 4;
    rdev->wiphy.frag_threshold = (u32)-1;
    rdev->wiphy.rts_threshold = (u32)-1;
    rdev->wiphy.coverage_class = 0;
    rdev->wiphy.max_num_csa_counters = 1;
    rdev->wiphy.max_sched_scan_plans = 1;
    rdev->wiphy.max_sched_scan_plan_interval = U32_MAX;

    return &rdev->wiphy;
}

void wiphy_free(struct wiphy *wiphy)
{
    struct rtw89_cfg80211_rdev *rdev;
    unsigned long flags;

    if (!wiphy)
        return;
    rdev = wiphy_to_rdev(wiphy);

    cancel_work_sync(&rdev->reg_work);
    cancel_work_sync(&rdev->wiphy_work);
    destroy_workqueue(rdev->wq);
    spin_lock_irqsave(&rdev->wiphy_work_lock, flags);
    while (!list_empty(&rdev->wiphy_work_list))
        list_del_init(rdev->wiphy_work_list.next);
    spin_unlock_irqrestore(&rdev->wiphy_work_lock, flags);

    spin_lock_destroy(&rdev->wiphy_work_lock);
    mutex_destroy(&wiphy->mtx);
    kfree(rdev->alloc_base);
}

/* ------------------------------------------------------------------ */
/*  wiphy work (runs with the wiphy mutex held)                         */
/* ------------------------------------------------------------------ */

/*
 * Same model as net/wireless/core.c: one runner work item per wiphy and a FIFO
 * of pending wiphy_works. The runner takes the wiphy mutex and runs one work
 * per pass so other lock waiters get a turn. cancel/flush are called with the
 * mutex held, so the work in question is not running; they only unlink it or
 * run it inline and never wait.
 */
static void rtw89_cfg80211_wiphy_work(struct work_struct *work)
{
    struct rtw89_cfg80211_rdev *rdev =
        container_of(work, struct rtw89_cfg80211_rdev, wiphy_work);
    struct wiphy_work *wk;
    unsigned long flags;

    wiphy_lock(&rdev->wiphy);

    spin_lock_irqsave(&rdev->wiphy_work_lock, flags);
    wk = list_first_entry_or_null(&rdev->wiphy_work_list, struct wiphy_work, entry);
    if (wk) {
        list_del_init(&wk->entry);
        if (!list_empty(&rdev->wiphy_work_list))
            queue_work(rdev->wq, work);
    }
    spin_unlock_irqrestore(&rdev->wiphy_work_lock, flags);

    if (wk)
        wk->func(&rdev->wiphy, wk);

    wiphy_unlock(&rdev->wiphy);
}

/* Run pending works in order, up to and including @end (all of them if NULL). */
static void rtw89_cfg80211_process_wiphy_works(struct rtw89_cfg80211_rdev *rdev,
                                               struct wiphy_work *end)
{
    unsigned long flags;

    spin_lock_irqsave(&rdev->wiphy_work_lock, flags);
    while (!list_empty(&rdev->wiphy_work_list)) {
        struct wiphy_work *wk;

        wk = list_first_entry(&rdev->wiphy_work_list, struct wiphy_work, entry);
        list_del_init(&wk->entry);
        spin_unlock_irqrestore(&rdev->wiphy_work_lock, flags);

        wk->func(&rdev->wiphy, wk);

        spin_lock_irqsave(&rdev->wiphy_work_lock, flags);
        if (wk == end)
            break;
    }
    spin_unlock_irqrestore(&rdev->wiphy_work_lock, flags);
}

void wiphy_work_queue(struct wiphy *wiphy, struct wiphy_work *work)
{
    struct rtw89_cfg80211_rdev *rdev = wiphy_to_rdev(wiphy);
    unsigned long flags;

    spin_lock_irqsave(&rdev->wiphy_work_lock, flags);
    if (list_empty(&work->entry))
        list_add_tail(&work->entry, &rdev->wiphy_work_list);
    spin_unlock_irqrestore(&rdev->wiphy_work_lock, flags);

    queue_work(rdev->wq, &rdev->wiphy_work);
}

void wiphy_work_cancel(struct wiphy *wiphy, struct wiphy_work *work)
{
    struct rtw89_cfg80211_rdev *rdev = wiphy_to_rdev(wiphy);
    unsigned long flags;

    spin_lock_irqsave(&rdev->wiphy_work_lock, flags);
    if (!list_empty(&work->entry))
        list_del_init(&work->entry);
    spin_unlock_irqrestore(&rdev->wiphy_work_lock, flags);
}

void wiphy_work_flush(struct wiphy *wiphy, struct wiphy_work *work)
{
    struct rtw89_cfg80211_rdev *rdev = wiphy_to_rdev(wiphy);
    unsigned long flags;
    bool run;

    spin_lock_irqsave(&rdev->wiphy_work_lock, flags);
    run = !work || !list_empty(&work->entry);
    spin_unlock_irqrestore(&rdev->wiphy_work_lock, flags);

    if (run)
        rtw89_cfg80211_process_wiphy_works(rdev, work);
}

/* Timer stage of a delayed wiphy work: hand it to the runner. Never takes the
 * wiphy mutex, so waiting for it with the mutex held (cancel/flush) is safe. */
void wiphy_delayed_work_timer(struct timer_list *t)
{
    struct wiphy_delayed_work *dwork = timer_container_of(dwork, t, timer);

    wiphy_work_queue(dwork->wiphy, &dwork->work);
}

void wiphy_delayed_work_queue(struct wiphy *wiphy, struct wiphy_delayed_work *dwork,
                              unsigned long delay)
{
    if (!delay) {
        del_timer(&dwork->timer);
        wiphy_work_queue(wiphy, &dwork->work);
        return;
    }

    dwork->wiphy = wiphy;
    mod_timer(&dwork->timer, jiffies + delay);
}

void wiphy_delayed_work_cancel(struct wiphy *wiphy, struct wiphy_delayed_work *dwork)
{
    del_timer_sync(&dwork->timer);
    wiphy_work_cancel(wiphy, &dwork->work);
}

void wiphy_delayed_work_flush(struct wiphy *wiphy, struct wiphy_delayed_work *dwork)
{
    del_timer_sync(&dwork->timer);
    wiphy_work_flush(wiphy, &dwork->work);
}

bool wiphy_delayed_work_pending(struct wiphy *wiphy, struct wiphy_delayed_work *dwork)
{
    return timer_pending(&dwork->timer);
}

/* ------------------------------------------------------------------ */
/*  Regulatory                                                          */
/* ------------------------------------------------------------------ */

/*
 * There is no regulatory database here. A driver hint is echoed back through
 * wiphy->reg_notifier, as the Linux core does once it has accepted the hint;
 * the driver then applies its own per-country TX power tables. The notifier
 * takes the wiphy mutex itself, so it must run from a work item.
 */
static void rtw89_cfg80211_reg_work(struct work_struct *work)
{
    struct rtw89_cfg80211_rdev *rdev =
        container_of(work, struct rtw89_cfg80211_rdev, reg_work);

    if (rdev->wiphy.reg_notifier)
        rdev->wiphy.reg_notifier(&rdev->wiphy, &rdev->reg_request);
}

/*
 * Registration. On Linux this is where the regulatory core first tells the
 * driver which domain is in force (the world domain "00" until something sets
 * a country), synchronously and before wiphy_register() returns. rtw89 depends
 * on that: its notifier is what initialises rtwdev->regulatory.regd, which the
 * rest of probe then dereferences.
 */
/*
 * The world regulatory domain ("00" in the Linux regulatory database), which
 * is what applies until a country is known: 2.4 GHz channels 1-11 as usual,
 * 12-14 and all of 5/6 GHz listen-only (NO_IR: no probe requests, no
 * beaconing), radar detection required on 5250-5730 MHz, and nothing above
 * 5835 MHz. The driver decides active vs passive scanning per channel from
 * these flags.
 */
static void rtw89_cfg80211_apply_world_regdom(struct wiphy *wiphy)
{
    struct ieee80211_supported_band *sband;
    struct ieee80211_channel *chan;
    int band, i;

    for (band = 0; band < NUM_NL80211_BANDS; band++) {
        sband = wiphy->bands[band];
        if (!sband)
            continue;

        for (i = 0; i < sband->n_channels; i++) {
            u32 freq, flags = 0;

            chan = &sband->channels[i];
            freq = chan->center_freq;
            chan->orig_flags = chan->flags;

            switch (band) {
            case NL80211_BAND_2GHZ:
                if (freq == 2484)
                    flags = IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_NO_OFDM;
                else if (freq > 2462)
                    flags = IEEE80211_CHAN_NO_IR;
                break;
            case NL80211_BAND_5GHZ:
                if (freq > 5825) {
                    flags = IEEE80211_CHAN_DISABLED;
                    break;
                }
                flags = IEEE80211_CHAN_NO_IR;
                if (freq >= 5260 && freq <= 5720)
                    flags |= IEEE80211_CHAN_RADAR;
                break;
            default:
                flags = IEEE80211_CHAN_NO_IR;
                break;
            }

            chan->flags |= flags;
            chan->max_reg_power = 20;
            if (!chan->max_power || chan->max_power > chan->max_reg_power)
                chan->max_power = chan->max_reg_power;
        }
    }
}

int wiphy_register(struct wiphy *wiphy)
{
    struct rtw89_cfg80211_rdev *rdev = wiphy_to_rdev(wiphy);

    rtw89_cfg80211_apply_world_regdom(wiphy);

    memset(&rdev->reg_request, 0, sizeof(rdev->reg_request));
    rdev->reg_request.initiator = NL80211_REGDOM_SET_BY_CORE;
    rdev->reg_request.alpha2[0] = '0';
    rdev->reg_request.alpha2[1] = '0';
    rdev->reg_request.dfs_region = NL80211_DFS_UNSET;

    if (wiphy->reg_notifier)
        wiphy->reg_notifier(wiphy, &rdev->reg_request);
    return 0;
}

void wiphy_unregister(struct wiphy *wiphy)
{
    struct rtw89_cfg80211_rdev *rdev = wiphy_to_rdev(wiphy);

    cancel_work_sync(&rdev->reg_work);
}

int regulatory_hint(struct wiphy *wiphy, const char *alpha2)
{
    struct rtw89_cfg80211_rdev *rdev;

    if (WARN_ON(!alpha2 || !wiphy))
        return -EINVAL;
    rdev = wiphy_to_rdev(wiphy);

    wiphy->regulatory_flags &= ~REGULATORY_CUSTOM_REG;

    rdev->reg_request.alpha2[0] = alpha2[0];
    rdev->reg_request.alpha2[1] = alpha2[1];
    rdev->reg_request.alpha2[2] = '\0';
    rdev->reg_request.initiator = NL80211_REGDOM_SET_BY_DRIVER;
    rdev->reg_request.dfs_region = NL80211_DFS_UNSET;

    queue_work(rdev->wq, &rdev->reg_work);
    return 0;
}

/*
 * regulatory_hint_country_ie() on association, restore_regulatory_settings()
 * on leaving (@alpha2 NULL): as in Linux, the access point's Country element
 * says which country the station is in while it is connected, and the driver's
 * notifier picks its TX power tables for it. Without it rtw89 stays on its
 * worldwide tables, 4.5 dB below the Canadian or US ones on 2.4 GHz.
 * Under the wiphy mutex; the notifier runs from reg_work.
 */
void rtw89_cfg80211_country_ie(struct wiphy *wiphy, const u8 *alpha2)
{
    struct rtw89_cfg80211_rdev *rdev = wiphy_to_rdev(wiphy);
    struct regulatory_request *req = &rdev->reg_request;
    char a0 = alpha2 ? alpha2[0] : '0', a1 = alpha2 ? alpha2[1] : '0';

    if (wiphy->regulatory_flags & REGULATORY_COUNTRY_IE_IGNORE)
        return;
    /* "XX" and the like: not a country the tables know */
    if (alpha2 && (a0 < 'A' || a0 > 'Z' || a1 < 'A' || a1 > 'Z'))
        return;
    if (req->alpha2[0] == a0 && req->alpha2[1] == a1)
        return;
    req->alpha2[0] = a0;
    req->alpha2[1] = a1;
    req->alpha2[2] = '\0';
    req->initiator = alpha2 ? NL80211_REGDOM_SET_BY_COUNTRY_IE : NL80211_REGDOM_SET_BY_CORE;
    req->dfs_region = NL80211_DFS_UNSET;
    queue_work(rdev->wq, &rdev->reg_work);
}

/* ------------------------------------------------------------------ */
/*  rfkill: no hardware kill switch handling on macOS                   */
/* ------------------------------------------------------------------ */

void wiphy_rfkill_set_hw_state_reason(struct wiphy *wiphy, bool blocked,
                                      enum rfkill_hard_block_reasons reason)
{
}

void wiphy_rfkill_start_polling(struct wiphy *wiphy)
{
}

/* ------------------------------------------------------------------ */
/*  Frame / channel helpers (net/wireless/util.c, chan.c, scan.c)       */
/* ------------------------------------------------------------------ */

/* Bridge-Tunnel header (for EtherTypes 80F3 and 8137) */
const unsigned char bridge_tunnel_header[6] __aligned(2) =
    { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0xf8 };

const unsigned char rfc1042_header[6] __aligned(2) =
    { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };

unsigned int __attribute_const__ ieee80211_hdrlen(__le16 fc)
{
    unsigned int hdrlen = 24;

    if (ieee80211_is_ext(fc))
        return 4;

    if (ieee80211_is_data(fc)) {
        if (ieee80211_has_a4(fc))
            hdrlen = 30;
        if (ieee80211_is_data_qos(fc)) {
            hdrlen += IEEE80211_QOS_CTL_LEN;
            if (ieee80211_has_order(fc))
                hdrlen += IEEE80211_HT_CTL_LEN;
        }
        return hdrlen;
    }

    if (ieee80211_is_mgmt(fc)) {
        if (ieee80211_has_order(fc))
            hdrlen += IEEE80211_HT_CTL_LEN;
        return hdrlen;
    }

    if (ieee80211_is_ctl(fc)) {
        /* ACK and CTS are 10 bytes, all other control frames 16. */
        if ((fc & cpu_to_le16(0x00E0)) == cpu_to_le16(0x00C0))
            hdrlen = 10;
        else
            hdrlen = 16;
    }

    return hdrlen;
}

u32 ieee80211_channel_to_freq_khz(int chan, enum nl80211_band band)
{
    /* 802.11 17.3.8.3.2 and Annex J; channel numbers overlap between bands. */
    if (chan <= 0)
        return 0;

    switch (band) {
    case NL80211_BAND_2GHZ:
    case NL80211_BAND_LC:
        if (chan == 14)
            return MHZ_TO_KHZ(2484);
        else if (chan < 14)
            return MHZ_TO_KHZ(2407 + chan * 5);
        break;
    case NL80211_BAND_5GHZ:
        if (chan >= 182 && chan <= 196)
            return MHZ_TO_KHZ(4000 + chan * 5);
        else
            return MHZ_TO_KHZ(5000 + chan * 5);
        break;
    case NL80211_BAND_6GHZ:
        if (chan == 2)
            return MHZ_TO_KHZ(5935);
        if (chan <= 233)
            return MHZ_TO_KHZ(5950 + chan * 5);
        break;
    case NL80211_BAND_60GHZ:
        if (chan < 7)
            return MHZ_TO_KHZ(56160 + chan * 2160);
        break;
    case NL80211_BAND_S1GHZ:
        return 902000 + chan * 500;
    default:
        break;
    }
    return 0;
}

int ieee80211_freq_khz_to_channel(u32 freq)
{
    freq = KHZ_TO_MHZ(freq);

    if (freq == 2484)
        return 14;
    else if (freq < 2484)
        return (freq - 2407) / 5;
    else if (freq >= 4910 && freq <= 4980)
        return (freq - 4000) / 5;
    else if (freq < 5925)
        return (freq - 5000) / 5;
    else if (freq == 5935)
        return 2;
    else if (freq <= 45000) /* DMG band lower limit */
        return (freq - 5950) / 5;
    else if (freq >= 58320 && freq <= 70200)
        return (freq - 56160) / 2160;
    else
        return 0;
}

void cfg80211_chandef_create(struct cfg80211_chan_def *chandef,
                             struct ieee80211_channel *chan,
                             enum nl80211_channel_type chan_type)
{
    if (WARN_ON(!chan))
        return;

    *chandef = (struct cfg80211_chan_def) {
        .chan = chan,
        .freq1_offset = chan->freq_offset,
    };

    switch (chan_type) {
    case NL80211_CHAN_NO_HT:
        chandef->width = NL80211_CHAN_WIDTH_20_NOHT;
        chandef->center_freq1 = chan->center_freq;
        break;
    case NL80211_CHAN_HT20:
        chandef->width = NL80211_CHAN_WIDTH_20;
        chandef->center_freq1 = chan->center_freq;
        break;
    case NL80211_CHAN_HT40PLUS:
        chandef->width = NL80211_CHAN_WIDTH_40;
        chandef->center_freq1 = chan->center_freq + 10;
        break;
    case NL80211_CHAN_HT40MINUS:
        chandef->width = NL80211_CHAN_WIDTH_40;
        chandef->center_freq1 = chan->center_freq - 10;
        break;
    default:
        WARN_ON(1);
    }
}

const struct element *
cfg80211_find_elem_match(u8 eid, const u8 *ies, unsigned int len,
                         const u8 *match, unsigned int match_len,
                         unsigned int match_offset)
{
    const struct element *elem;

    for_each_element_id(elem, eid, ies, len) {
        if (elem->datalen >= match_offset + match_len &&
            !memcmp(elem->data + match_offset, match, match_len))
            return elem;
    }

    return NULL;
}

const struct element *cfg80211_find_vendor_elem(unsigned int oui, int oui_type,
                                                const u8 *ies, unsigned int len)
{
    const struct element *elem;
    u8 match[] = { oui >> 16, oui >> 8, oui, oui_type };
    int match_len = (oui_type < 0) ? 3 : sizeof(match);

    if (WARN_ON(oui_type > 0xff))
        return NULL;

    elem = cfg80211_find_elem_match(WLAN_EID_VENDOR_SPECIFIC, ies, len,
                                    match, match_len, 0);
    if (!elem || elem->datalen < 4)
        return NULL;

    return elem;
}

struct ieee80211_channel *ieee80211_get_channel_khz(struct wiphy *wiphy, u32 freq)
{
    struct ieee80211_supported_band *sband;
    int band, i;

    for (band = 0; band < NUM_NL80211_BANDS; band++) {
        sband = wiphy->bands[band];
        if (!sband)
            continue;
        for (i = 0; i < sband->n_channels; i++)
            if (ieee80211_channel_to_khz(&sband->channels[i]) == freq)
                return &sband->channels[i];
    }
    return NULL;
}

int cfg80211_get_ies_channel_number(const u8 *ie, size_t ielen,
                                    enum nl80211_band band)
{
    const struct element *elem;

    if (band == NL80211_BAND_S1GHZ)
        return -1;

    elem = cfg80211_find_elem(WLAN_EID_DS_PARAMS, ie, ielen);
    if (elem && elem->datalen == 1)
        return elem->data[0];

    elem = cfg80211_find_elem(WLAN_EID_HT_OPERATION, ie, ielen);
    if (elem && elem->datalen >= sizeof(struct ieee80211_ht_operation)) {
        const struct ieee80211_ht_operation *htop = (const void *)elem->data;

        return htop->primary_chan;
    }

    return -1;
}

/*
 * cfg80211 keeps the scan result table on Linux; here IO80211 does. rtw89 only
 * walks it to look for neighbouring BSSes that cannot tolerate narrow-band
 * HE RUs (UL OFDMA). With nothing to walk, that check finds no such BSS.
 */
void cfg80211_bss_iter(struct wiphy *wiphy, struct cfg80211_chan_def *chandef,
                       void (*iter)(struct wiphy *wiphy, struct cfg80211_bss *bss,
                                    void *data),
                       void *iter_data)
{
}
