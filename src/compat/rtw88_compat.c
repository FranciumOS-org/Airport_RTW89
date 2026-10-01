// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
// Compatibility layer implementation for rtw88 macOS port.
// Compiled as C with -mkernel; no userspace headers allowed.

#include "rtw88_compat.h"
#include <stdarg.h>
#include <kern/thread_call.h>

/* ------------------------------------------------------------------ */
/*  Logging                                                             */
/* ------------------------------------------------------------------ */

int rtw88_log_level = KERN_DEBUG;

struct task_struct *__rtw88_current_task = NULL;

static IOSimpleLock *rtw88_log_lock = NULL;
static char rtw88_log_ring[8192];
static uint32_t rtw88_log_head = 0;
static uint32_t rtw88_log_tail = 0;

static void rtw88_log_append(const char *msg)
{
    if (!rtw88_log_lock) return;
    IOSimpleLockLock(rtw88_log_lock);
    while (*msg) {
        rtw88_log_ring[rtw88_log_head] = *msg++;
        rtw88_log_head = (rtw88_log_head + 1) % sizeof(rtw88_log_ring);
        if (rtw88_log_head == rtw88_log_tail) {
            rtw88_log_tail = (rtw88_log_tail + 1) % sizeof(rtw88_log_ring);
        }
    }
    IOSimpleLockUnlock(rtw88_log_lock);
}

void rtw88_printk(int level, const char *fmt, ...)
{
    if (level > rtw88_log_level) return;

    static const char * const level_str[] = { "ERR", "WARN", "INFO", "DBG" };
    const char *ls = (level >= 0 && level <= 3) ? level_str[level] : "???";

    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    IOLog("[rtw88 %s] %s", ls, buf);
    
    char ring_msg[512];
    snprintf(ring_msg, sizeof(ring_msg), "[rtw88 %s] %s", ls, buf);
    rtw88_log_append(ring_msg);
}

void rtw88_dev_printk(int level, struct device *dev, const char *fmt, ...)
{
    if (level > rtw88_log_level) return;

    static const char * const level_str[] = { "ERR", "WARN", "INFO", "DBG" };
    const char *ls = (level >= 0 && level <= 3) ? level_str[level] : "???";
    const char *name = dev ? (dev->name ? dev->name : "?") : "?";

    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    IOLog("[rtw88 %s %s] %s", ls, name, buf);
    
    char ring_msg[512];
    snprintf(ring_msg, sizeof(ring_msg), "[rtw88 %s %s] %s", ls, name, buf);
    rtw88_log_append(ring_msg);
    
    /* Do NOT sleep here — IOSleep in a hot error path (e.g. "failed to write
     * TX skb to HCI" called from the output queue) stalls the IOKit thread
     * for 2 s per error, which both degrades throughput and makes subsequent
     * errors compound by holding locks. */
}

void rtw88_hex_dump(const char *prefix, const void *buf, size_t len)
{
    const u8 *p = (const u8 *)buf;
    IOLog("[rtw88] %s (%zu bytes):\n", prefix, len);
    for (size_t i = 0; i < len; i += 16) {
        char line[80];
        int pos = 0;
        pos += snprintf(line + pos, (int)(sizeof(line) - pos), "  %04zx: ", i);
        for (size_t j = i; j < i + 16 && j < len; j++)
            pos += snprintf(line + pos, (int)(sizeof(line) - pos), "%02x ", p[j]);
        IOLog("%s\n", line);
    }
}

/* ------------------------------------------------------------------ */
/*  Symbols not exported by macOS 15+ KPIs — provided internally       */
/* ------------------------------------------------------------------ */

/* fls: declared extern in MacKernelSDK libkern.h but not KPI-exported */
int fls(unsigned int x)
{
    return x ? (32 - __builtin_clz(x)) : 0;
}

/* ------------------------------------------------------------------ */
/*  Global DMA / PCI / USB ops pointers                                 */
/* ------------------------------------------------------------------ */

struct rtw88_dma_alloc_ops *rtw88_dma_ops      = NULL;
struct pci_ops_rtw88       *rtw88_pci_io_ops   = NULL;
struct rtw88_usb_ops       *rtw88_usb_io_ops   = NULL;

/* Until station iteration is implemented, main.c limits the watchdog to
 * chip-local thermal power tracking. Full DIG/RA needs valid station stats. */
bool rtw88_disable_watchdog_work = true;

/* ------------------------------------------------------------------ */
/*  Workqueue implementation (kernel threads + IOLock)                  */
/* ------------------------------------------------------------------ */

/* One state lock also serializes transfers/cancellation across queues. */
static IOLock *work_state_lock;
struct workqueue_struct *system_wq      = NULL;
struct workqueue_struct *system_long_wq = NULL;
/* Keep PCI threaded IRQ + NAPI off the control-plane workqueues. */
static struct workqueue_struct *g_datapath_wq = NULL;

static void wq_thread_fn(void *arg, wait_result_t wr)
{
    struct workqueue_struct *wq = (struct workqueue_struct *)arg;

    IOLockLock(wq->lock);
    while (wq->running || !list_empty(&wq->queue)) {
        if (list_empty(&wq->queue)) {
            IOLockSleep(wq->lock, &wq->queue, THREAD_UNINT);
            continue;
        }
        struct work_struct *work = container_of(wq->queue.next,
                                                struct work_struct, entry);
        list_del_init(&work->entry);
        work->pending = 0;
        work->executing = true;
        wq->active++;
        IOLockUnlock(wq->lock);
        if (work->func) work->func(work);
        IOLockLock(wq->lock);
        work->executing = false;
        wq->active--;
        IOLockWakeup(wq->lock, work, false);
        IOLockWakeup(wq->lock, &wq->queue, false);
    }

    wq->done = 1;
    IOLockWakeup(wq->lock, (void *)&wq->done, false);
    IOLockUnlock(wq->lock);
    thread_terminate(current_thread());
}

struct workqueue_struct *alloc_workqueue(const char *name, unsigned int flags,
                                          int max_active)
{
    struct workqueue_struct *wq =
        (struct workqueue_struct *)IOMalloc(sizeof(*wq));
    if (!wq) return NULL;
    bzero(wq, sizeof(*wq));
    strlcpy(wq->name, name ? name : "wq", sizeof(wq->name));
    INIT_LIST_HEAD(&wq->queue);
    wq->lock    = work_state_lock;
    wq->running = 1;
    wq->done    = 0;
    if (!wq->lock) { IOFree(wq, sizeof(*wq)); return NULL; }

    kern_return_t kr = kernel_thread_start(wq_thread_fn, wq, &wq->thread);
    if (kr != KERN_SUCCESS) {
        IOFree(wq, sizeof(*wq));
        return NULL;
    }
    thread_deallocate(wq->thread);
    return wq;
}

struct workqueue_struct *alloc_ordered_workqueue(const char *name,
                                                  unsigned int flags)
{
    return alloc_workqueue(name, flags, 1);
}

void destroy_workqueue(struct workqueue_struct *wq)
{
    if (!wq) return;
    IOLockLock(wq->lock);
    wq->running = 0;
    IOLockWakeup(wq->lock, &wq->queue, false);
    while (!wq->done)
        IOLockSleep(wq->lock, (void *)&wq->done, THREAD_UNINT);
    IOLockUnlock(wq->lock);
    IOFree(wq, sizeof(*wq));
}

/* All work state below is protected by work_state_lock. Owners must stop
 * producers and cancel delayed work before destroying the target queue. */
static bool queue_work_locked(struct workqueue_struct *wq, struct work_struct *work)
{
    if (!wq->running || work->pending || work->canceling)
        return false;
    if (work->executing && work->wq != wq)
        return false;
    work->wq = wq;
    work->pending = 1;
    list_add_tail(&work->entry, &wq->queue);
    IOLockWakeup(work_state_lock, &wq->queue, false);
    return true;
}

bool queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
    if (!wq || !work || !work_state_lock) return false;
    IOLockLock(work_state_lock);
    bool queued = queue_work_locked(wq, work);
    IOLockUnlock(work_state_lock);
    return queued;
}

bool schedule_work(struct work_struct *work)
{
    return queue_work(system_wq, work);
}

void rtw88_delayed_work_timer_fn(struct timer_list *t)
{
    struct delayed_work *dwork = from_timer(dwork, t, timer);
    IOLockLock(work_state_lock);
    if (dwork->work.pending == 2) {
        dwork->work.pending = 0;
        queue_work_locked(dwork->work.wq, &dwork->work);
    }
    IOLockWakeup(work_state_lock, &dwork->work, false);
    IOLockUnlock(work_state_lock);
}

bool queue_delayed_work(struct workqueue_struct *wq,
                       struct delayed_work *dwork, unsigned long delay)
{
    if (!wq || !dwork || !work_state_lock) return false;
    if (!delay) return queue_work(wq, &dwork->work);
    IOLockLock(work_state_lock);
    struct work_struct *work = &dwork->work;
    if (!wq->running || work->pending || work->canceling ||
        (work->executing && work->wq != wq)) {
        IOLockUnlock(work_state_lock);
        return false;
    }
    work->wq = wq;
    work->pending = 2;
    mod_timer(&dwork->timer, jiffies + delay);
    if (!dwork->timer.call) work->pending = 0;
    bool queued = work->pending != 0;
    IOLockUnlock(work_state_lock);
    return queued;
}

bool schedule_delayed_work(struct delayed_work *dwork, unsigned long delay)
{
    return queue_delayed_work(system_wq, dwork, delay);
}

void flush_workqueue(struct workqueue_struct *wq)
{
    if (!wq || !work_state_lock) return;
    IOLockLock(work_state_lock);
    while (!list_empty(&wq->queue) || wq->active)
        IOLockSleep(work_state_lock, &wq->queue, THREAD_UNINT);
    IOLockUnlock(work_state_lock);
}

static bool remove_pending_work(struct work_struct *work)
{
    bool pending = work->pending != 0;
    if (work->pending == 1) list_del_init(&work->entry);
    work->pending = 0;
    if (pending && work->wq)
        IOLockWakeup(work_state_lock, &work->wq->queue, false);
    IOLockWakeup(work_state_lock, work, false);
    return pending;
}

bool cancel_work_sync(struct work_struct *work)
{
    if (!work || !work_state_lock) return false;
    IOLockLock(work_state_lock);
    work->canceling = true;
    bool pending = remove_pending_work(work);
    while (work->executing)
        IOLockSleep(work_state_lock, work, THREAD_UNINT);
    work->canceling = false;
    IOLockUnlock(work_state_lock);
    return pending;
}

bool cancel_delayed_work_sync(struct delayed_work *dwork)
{
    if (!dwork || !work_state_lock) return false;
    IOLockLock(work_state_lock);
    dwork->work.canceling = true;
    bool pending = remove_pending_work(&dwork->work);
    IOLockUnlock(work_state_lock);
    /* Never wait for a timer callback while holding its state lock. */
    del_timer_sync(&dwork->timer);
    IOLockLock(work_state_lock);
    while (dwork->work.executing)
        IOLockSleep(work_state_lock, &dwork->work, THREAD_UNINT);
    dwork->work.canceling = false;
    IOLockUnlock(work_state_lock);
    return pending;
}

bool cancel_delayed_work(struct delayed_work *dwork)
{
    if (!dwork || !work_state_lock) return false;
    IOLockLock(work_state_lock);
    bool pending = remove_pending_work(&dwork->work);
    del_timer(&dwork->timer);
    IOLockUnlock(work_state_lock);
    return pending;
}

void flush_work(struct work_struct *work)
{
    if (!work || !work_state_lock) return;
    IOLockLock(work_state_lock);
    while (work->pending || work->executing)
        IOLockSleep(work_state_lock, work, THREAD_UNINT);
    IOLockUnlock(work_state_lock);
}

void flush_scheduled_work(void)
{
    flush_workqueue(system_wq);
}

/* ------------------------------------------------------------------ */
/*  Timer implementation (thread_call)                                  */
/* ------------------------------------------------------------------ */

static void timer_call_fn(thread_call_param_t p0, thread_call_param_t p1)
{
    struct timer_list *timer = (struct timer_list *)p0;
    (void)p1;
    /* Only fire if still marked active (del_timer may have raced) */
    if (timer->active) {
        timer->active = 0;
        if (timer->function) timer->function(timer);
    }
}

void timer_setup(struct timer_list *timer,
                 void (*func)(struct timer_list *t),
                 unsigned int flags)
{
    timer->function = func;
    timer->expires  = 0;
    timer->active   = 0;
    timer->call = NULL;
}

int mod_timer(struct timer_list *timer, unsigned long expires)
{
    int was_active = timer->active;

    if (!timer->call)
        timer->call = thread_call_allocate(timer_call_fn,
                                            (thread_call_param_t)timer);

    if (!timer->call) return was_active;

    long delay_ms = (long)expires - (long)jiffies;
    if (delay_ms <= 0) delay_ms = 1;

    uint64_t deadline;
    clock_interval_to_deadline((uint32_t)delay_ms, kMillisecondScale, &deadline);
    timer->expires = expires;
    timer->active  = 1;
    thread_call_enter_delayed(timer->call, deadline);
    return was_active;
}

int del_timer(struct timer_list *timer)
{
    int was_active = timer->active;
    if (timer->call) thread_call_cancel(timer->call);
    timer->active = 0;
    return was_active;
}

int del_timer_sync(struct timer_list *timer)
{
    timer->active = 0;
    if (timer->call) {
        thread_call_cancel_wait(timer->call);
        thread_call_free(timer->call);
        timer->call = NULL;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  mac80211 callbacks                                                  */
/* ------------------------------------------------------------------ */

#ifndef RTW89_MACOS /* rtw88 kext glue */
struct rtw88_hw_callbacks {
    void (*rx_frame)(void *kext_hw, struct sk_buff *skb);
    void (*tx_status)(void *kext_hw, struct sk_buff *skb);
    void (*scan_done)(void *kext_hw, bool aborted);
};

static struct rtw88_hw_callbacks *g_hw_cbs = NULL;
static void *g_kext_hw = NULL;

/* Forward declaration — defined later in ieee80211_alloc_hw section */
static struct ieee80211_hw *g_rtw88_hw;
#endif /* !RTW89_MACOS */

irq_handler_t g_irq_handler = NULL;
irq_handler_t g_irq_thread_fn = NULL;
void *g_irq_dev_id = NULL;

/* XNU thread_call can be re-entered while an earlier invocation is still
 * running. Linux NAPI/threaded IRQ relies on single-flight execution, so use
 * one ordered queue for the PCI datapath instead. */
static struct work_struct g_irq_work;
static bool g_irq_work_initialized = false;

#ifndef RTW89_MACOS /* rtw88 kext glue */
/* Single active VIF — registered by the kext after add_interface so that
 * ieee80211_iterate_active_interfaces_atomic can deliver the iterator to
 * rtw88's internal callbacks (e.g. rtw_build_rsvd_page_iter). */
static struct ieee80211_vif *g_rtw88_vif = NULL;

void rtw88_register_vif(struct ieee80211_vif *vif)   { g_rtw88_vif = vif; }
void rtw88_unregister_vif(void)                       { g_rtw88_vif = NULL; }
#endif /* !RTW89_MACOS */

/* Kext hook fired after TX ISR has reclaimed descriptors. It runs on the
 * ordered datapath worker, outside the Realtek IRQ handler itself. */
static void (*g_tx_resume_cb)(void) = NULL;
void rtw88_set_tx_resume_cb(void (*cb)(void)) { g_tx_resume_cb = cb; }

static void rtw88_irq_work_fn(struct work_struct *work)
{
    (void)work;
    if (g_irq_thread_fn && g_irq_dev_id)
        g_irq_thread_fn(0, g_irq_dev_id);

    /* TX completions may have opened the BE ring. Wake an output queue that
     * was deliberately stalled before rtw_tx() could start dropping skbs. */
    if (g_tx_resume_cb)
        g_tx_resume_cb();
}

#ifndef RTW89_MACOS /* rtw88 driver internals */
/* BE-ring free-slot count for kext flow control. */
extern u32 rtw88_be_ring_avail(struct rtw_dev *rtwdev);
u32 rtw88_be_tx_avail(void)
{
    if (!g_irq_dev_id) return 0;
    return rtw88_be_ring_avail((struct rtw_dev *)g_irq_dev_id);
}
#endif /* !RTW89_MACOS */

int rtw88_devm_request_threaded_irq(struct device *dev, unsigned int irq,
        irq_handler_t handler, irq_handler_t thread_fn,
        unsigned long flags, const char *name, void *dev_id)
{
    (void)dev; (void)irq; (void)flags; (void)name;
    g_irq_handler = handler;
    g_irq_thread_fn = thread_fn;
    g_irq_dev_id = dev_id;
    if (!g_irq_work_initialized) {
        INIT_WORK(&g_irq_work, rtw88_irq_work_fn);
        g_irq_work_initialized = true;
    }
    return 0;
}

void rtw88_devm_free_irq(struct device *dev, unsigned int irq, void *dev_id)
{
    (void)dev; (void)irq; (void)dev_id;
    if (g_irq_work_initialized)
        cancel_work_sync(&g_irq_work);
    g_irq_handler = NULL;
    g_irq_thread_fn = NULL;
    g_irq_dev_id = NULL;
}

void rtw88_trigger_interrupt(void)
{
    if (!g_irq_handler || !g_irq_dev_id)
        return;

    int ret = g_irq_handler(0, g_irq_dev_id);
    if (ret == IRQ_WAKE_THREAD && g_irq_work_initialized && g_datapath_wq)
        queue_work(g_datapath_wq, &g_irq_work);
}

void rtw88_synchronize_irq(void)
{
    if (g_irq_work_initialized)
        flush_work(&g_irq_work);
}

static void rtw88_napi_work_fn(struct work_struct *work)
{
    struct napi_struct *napi = container_of(work, struct napi_struct, work);
    if (!napi || !napi->enabled || !napi->poll)
        return;

    int done = napi->poll(napi, napi->weight);

    /* A full budget means Linux would leave NAPI scheduled. Requeue the same
     * work item; the ordered queue guarantees the polls never overlap. */
    if (napi->enabled && done >= napi->weight && g_datapath_wq)
        queue_work(g_datapath_wq, &napi->work);
}

void rtw88_netif_napi_add(struct net_device *dev, struct napi_struct *napi,
                           int (*poll_fn)(struct napi_struct *, int))
{
    if (!napi) return;
    napi->dev = dev;
    napi->poll = poll_fn;
    napi->weight = 64;
    napi->enabled = false;
    INIT_WORK(&napi->work, rtw88_napi_work_fn);
}

void rtw88_napi_enable(struct napi_struct *napi)
{
    if (napi) napi->enabled = true;
}

void rtw88_napi_synchronize(struct napi_struct *napi)
{
    if (napi)
        flush_work(&napi->work);
}

void rtw88_napi_disable(struct napi_struct *napi)
{
    if (!napi) return;
    napi->enabled = false;
    cancel_work_sync(&napi->work);
}

void rtw88_netif_napi_del(struct napi_struct *napi)
{
    if (!napi) return;
    rtw88_napi_disable(napi);
    napi->poll = NULL;
    napi->dev = NULL;
}

void rtw88_napi_schedule(struct napi_struct *napi)
{
    if (napi && napi->enabled && napi->poll && g_datapath_wq)
        queue_work(g_datapath_wq, &napi->work);
}

#ifndef RTW89_MACOS /* rtw88 mini-mac80211; rtw89 has its own in src/compat_rtw89 */
void rtw88_set_hw_callbacks(struct rtw88_hw_callbacks *cbs, void *kext_hw)
{
    g_hw_cbs  = cbs;
    g_kext_hw = kext_hw;
    /* Populate the kext_hw back-pointer in the hw struct so all callbacks
     * that dereference hw->kext_hw actually reach the RTW88IEEE80211 object.
     * Without this every callback fires with NULL and silently does nothing. */
    if (g_rtw88_hw)
        g_rtw88_hw->kext_hw = kext_hw;
}

void ieee80211_rx_irqsafe(struct ieee80211_hw *hw, struct sk_buff *skb)
{
    /* Linux mac80211 removes the trailing 802.11 FCS before handing frames to
     * normal RX processing when a driver advertises RX_INCLUDES_FCS.  This
     * port bypasses mac80211's RX core and forwards the rtw88 skb directly to
     * RTW88IEEE80211, so preserving the four CRC bytes changes the effective
     * ABI: data packets grow four bytes and, critically, AWDL action parsers
     * interpret the CRC as another TLV/trailing garbage.  rtw88 explicitly
     * sets RX_INCLUDES_FCS in rtw_register_hw(), therefore emulate the missing
     * mac80211 normalization here, once, for every RX consumer. */
    if (skb && hw && ieee80211_hw_check(hw, RX_INCLUDES_FCS) && skb->len >= 4)
        skb_trim(skb, skb->len - 4);

    /* Use g_kext_hw as fallback in case hw->kext_hw wasn't set yet */
    void *ctx = hw ? hw->kext_hw : NULL;
    if (!ctx) ctx = g_kext_hw;
    if (g_hw_cbs && g_hw_cbs->rx_frame)
        g_hw_cbs->rx_frame(ctx, skb);
    else
        kfree_skb(skb);
}

void ieee80211_rx_napi(struct ieee80211_hw *hw, struct ieee80211_sta *sta,
                       struct sk_buff *skb, struct napi_struct *napi)
{
    ieee80211_rx_irqsafe(hw, skb);
}

void ieee80211_tx_status_irqsafe(struct ieee80211_hw *hw, struct sk_buff *skb)
{
    void *ctx = hw ? hw->kext_hw : NULL;
    if (!ctx) ctx = g_kext_hw;
    if (g_hw_cbs && g_hw_cbs->tx_status)
        g_hw_cbs->tx_status(ctx, skb);
    else
        kfree_skb(skb);
}

void ieee80211_tx_status(struct ieee80211_hw *hw, struct sk_buff *skb)
{
    ieee80211_tx_status_irqsafe(hw, skb);
}

void ieee80211_free_txskb(struct ieee80211_hw *hw, struct sk_buff *skb)
{
    kfree_skb(skb);
}

void ieee80211_scan_completed(struct ieee80211_hw *hw,
                               struct cfg80211_scan_info *info)
{
    void *ctx = hw ? hw->kext_hw : NULL;
    if (!ctx) ctx = g_kext_hw;
    if (g_hw_cbs && g_hw_cbs->scan_done)
        g_hw_cbs->scan_done(ctx, info ? info->aborted : false);
}

void ieee80211_stop_queues(struct ieee80211_hw *hw)  {}
void ieee80211_wake_queues(struct ieee80211_hw *hw)  {}
void ieee80211_stop_queue(struct ieee80211_hw *hw, int q) {}
void ieee80211_wake_queue(struct ieee80211_hw *hw, int q) {}
void ieee80211_schedule_txq(struct ieee80211_hw *hw, struct ieee80211_txq *txq) {}

void ieee80211_connection_loss(struct ieee80211_vif *vif) {}
void ieee80211_beacon_loss(struct ieee80211_vif *vif) {}
void ieee80211_cqm_rssi_notify(struct ieee80211_vif *vif,
                                int event, s32 rssi_level, gfp_t gfp) {}

void ieee80211_iterate_active_interfaces_atomic(
    struct ieee80211_hw *hw, unsigned int iter_flags,
    void (*iterator)(void *data, u8 *mac, struct ieee80211_vif *vif),
    void *data)
{
    if (g_rtw88_vif && iterator)
        iterator(data, g_rtw88_vif->addr, g_rtw88_vif);
}

void ieee80211_iterate_active_interfaces(
    struct ieee80211_hw *hw, unsigned int iter_flags,
    void (*iterator)(void *data, u8 *mac, struct ieee80211_vif *vif),
    void *data)
{
    if (g_rtw88_vif && iterator)
        iterator(data, g_rtw88_vif->addr, g_rtw88_vif);
}

void ieee80211_iterate_stations_atomic(
    struct ieee80211_hw *hw,
    void (*iterator)(void *data, struct ieee80211_sta *sta),
    void *data) {}

void ieee80211_iter_keys(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
    void (*iter)(struct ieee80211_hw *, struct ieee80211_vif *,
                 struct ieee80211_sta *, struct ieee80211_key_conf *, void *),
    void *iter_data) {}

void ieee80211_iter_keys_rcu(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
    void (*iter)(struct ieee80211_hw *, struct ieee80211_vif *,
                 struct ieee80211_sta *, struct ieee80211_key_conf *, void *),
    void *iter_data) {}

struct sk_buff *ieee80211_tx_dequeue(struct ieee80211_hw *hw,
                                      struct ieee80211_txq *txq)
{ return NULL; }

struct sk_buff *ieee80211_tx_dequeue_ni(struct ieee80211_hw *hw,
                                         struct ieee80211_txq *txq)
{ return NULL; }

bool ieee80211_txq_may_transmit(struct ieee80211_hw *hw,
                                 struct ieee80211_txq *txq)
{ return false; }

void ieee80211_txq_schedule_start(struct ieee80211_hw *hw, u8 ac) {}
void ieee80211_txq_schedule_end(struct ieee80211_hw *hw, u8 ac)   {}
bool ieee80211_txq_is_last(struct ieee80211_hw *hw, struct ieee80211_txq *txq)
{ return true; }

struct sk_buff *ieee80211_beacon_get_tim(struct ieee80211_hw *hw,
                                          struct ieee80211_vif *vif,
                                          u16 *tim_offset, u16 *tim_length,
                                          u32 link_id)
{ return NULL; }

/*
 * Build an 802.11 Null Function frame (subtype 0x4, type Data) addressed
 * from STA to its AP.  Used by rtw88's reserved-page download so the
 * firmware can keep-alive the association during LPS.
 *
 * The caller (rtw_fill_rsvd_page_desc) prepends chip->tx_pkt_desc_sz bytes
 * via skb_push, so we must reserve headroom for that descriptor.
 */
struct sk_buff *ieee80211_nullfunc_get(struct ieee80211_hw *hw,
                                        struct ieee80211_vif *vif,
                                        int link_id, bool qos_ok)
{
    const u32 headroom = 64;   /* enough for any chip's tx_pkt_desc_sz */
    u32 hdr_len = qos_ok ? 26 : 24;
    struct sk_buff *skb;

    if (!vif) return NULL;

    skb = alloc_skb(headroom + hdr_len, GFP_ATOMIC);
    if (!skb) return NULL;
    skb_reserve(skb, headroom);

    u8 *p = skb_put_zero(skb, hdr_len);
    /* Frame Control: Data (type=2), Null (subtype=0x4 or 0xC for QoS-Null) */
    u8 subtype = qos_ok ? 0xC : 0x4;
    p[0] = (u8)((subtype << 4) | (2 << 2));   /* type=Data, subtype=Null */
    p[1] = 0x01;                               /* ToDS=1, FromDS=0 */
    /* Duration */
    p[2] = 0; p[3] = 0;
    /* Address 1 = BSSID (RA) */
    memcpy(&p[4], vif->bss_conf.bssid, 6);
    /* Address 2 = STA (TA = SA) */
    memcpy(&p[10], vif->addr, 6);
    /* Address 3 = BSSID (DA) */
    memcpy(&p[16], vif->bss_conf.bssid, 6);
    /* Sequence control = 0 */
    p[22] = 0; p[23] = 0;
    /* QoS Control (only for QoS-Null) */
    if (qos_ok) { p[24] = 0; p[25] = 0; }

    return skb;
}

/*
 * Build an 802.11 PS-Poll frame.  16 bytes total:
 *   FC(2) AID(2) BSSID(6) TA(6)
 * AID is carried in the duration field with the top two bits set (the
 * 802.11 "PS-Poll AID encoding").
 */
struct sk_buff *ieee80211_pspoll_get(struct ieee80211_hw *hw,
                                      struct ieee80211_vif *vif)
{
    const u32 headroom = 64;
    const u32 frame_len = 16;
    struct sk_buff *skb;

    if (!vif) return NULL;

    skb = alloc_skb(headroom + frame_len, GFP_ATOMIC);
    if (!skb) return NULL;
    skb_reserve(skb, headroom);

    u8 *p = skb_put_zero(skb, frame_len);
    /* FC: type=Control (1), subtype=PS-Poll (0xA) */
    p[0] = (u8)((0xA << 4) | (1 << 2));    /* 0xA4 */
    p[1] = 0x00;
    /* AID with top two bits set (per 802.11) */
    u16 aid = (u16)(vif->cfg.aid | 0xC000);
    p[2] = (u8)(aid & 0xFF);
    p[3] = (u8)(aid >> 8);
    /* BSSID */
    memcpy(&p[4], vif->bss_conf.bssid, 6);
    /* TA = our STA address */
    memcpy(&p[10], vif->addr, 6);

    return skb;
}

struct sk_buff *ieee80211_probereq_get(struct ieee80211_hw *hw,
                                        const u8 *src_addr,
                                        const u8 *ssid, size_t ssid_len,
                                        size_t tailroom)
{ return NULL; }

int  ieee80211_sta_ps_transition(struct ieee80211_sta *sta, bool start) { return 0; }
void ieee80211_sta_pspoll(struct ieee80211_sta *sta) {}
void ieee80211_sta_uapsd_trigger(struct ieee80211_sta *sta, u8 tid) {}

void ieee80211_chswitch_done(struct ieee80211_vif *vif, bool success,
                              unsigned int link_id) {}

int ieee80211_freq_to_channel(int freq)
{
    if (freq >= 2412 && freq <= 2484)
        return (freq - 2407) / 5;
    if (freq >= 5180 && freq <= 5825)
        return (freq - 5000) / 5;
    return 0;
}

int ieee80211_channel_to_frequency(int chan, enum nl80211_band band)
{
    if (band == NL80211_BAND_2GHZ)
        return 2407 + chan * 5;
    return 5000 + chan * 5;
}
#endif /* !RTW89_MACOS */

/* ------------------------------------------------------------------ */
/*  Workqueue extras                                                    */
/* ------------------------------------------------------------------ */

struct workqueue_struct *create_singlethread_workqueue(const char *name)
{
    return alloc_ordered_workqueue(name, 0);
}

#ifndef RTW89_MACOS /* rtw88 mini-mac80211 */
void ieee80211_queue_work(struct ieee80211_hw *hw, struct work_struct *work)
{
    if (hw && hw->priv) {
        struct workqueue_struct *wq = system_wq;
        queue_work(wq, work);
    }
}

void ieee80211_queue_delayed_work(struct ieee80211_hw *hw,
                                   struct delayed_work *dwork,
                                   unsigned long delay)
{
    queue_delayed_work(system_wq, dwork, delay);
}

/* ------------------------------------------------------------------ */
/*  mac80211 stubs                                                      */
/* ------------------------------------------------------------------ */

struct ieee80211_sta *ieee80211_find_sta(struct ieee80211_vif *vif,
                                          const u8 *addr)
{ (void)vif; (void)addr; return NULL; }

struct ieee80211_sta *ieee80211_find_sta_by_ifaddr(struct ieee80211_hw *hw,
                                                    const u8 *addr,
                                                    const u8 *localaddr)
{ (void)hw; (void)addr; (void)localaddr; return NULL; }

struct sk_buff *ieee80211_proberesp_get(struct ieee80211_hw *hw,
                                         struct ieee80211_vif *vif)
{ (void)hw; (void)vif; return NULL; }

void ieee80211_purge_tx_queue(struct ieee80211_hw *hw,
                               struct sk_buff_head *skbs)
{
    struct sk_buff *skb;
    while ((skb = skb_dequeue(skbs)) != NULL)
        kfree_skb(skb);
}

void ieee80211_restart_hw(struct ieee80211_hw *hw) { (void)hw; }

int ieee80211_start_tx_ba_session(struct ieee80211_sta *sta, u16 tid,
                                   u16 timeout)
{ (void)sta; (void)tid; (void)timeout; return -EOPNOTSUPP; }

void ieee80211_stop_tx_ba_cb_irqsafe(struct ieee80211_vif *vif,
                                      const u8 *ra, u16 tid)
{ (void)vif; (void)ra; (void)tid; }

void ieee80211_tx_info_clear_status(struct ieee80211_tx_info *info)
{
    int i;
    for (i = 0; i < IEEE80211_TX_MAX_RATES; i++) {
        info->status.rates[i].idx   = -1;
        info->status.rates[i].count = 0;
        info->status.rates[i].flags = 0;
    }
}

void ieee80211_txq_get_depth(struct ieee80211_txq *txq,
                              unsigned long *frame_cnt,
                              unsigned long *byte_cnt)
{
    if (frame_cnt) *frame_cnt = 0;
    if (byte_cnt)  *byte_cnt  = 0;
}

u8 ieee80211_vif_type_p2p(struct ieee80211_vif *vif)
{
    return vif ? (u8)vif->type : 0;
}

/* Single global hw pointer — forward-declared above, defined/initialized here. */
static struct ieee80211_hw *g_rtw88_hw = NULL;

void rtw88_register_hw(struct ieee80211_hw *hw) { g_rtw88_hw = hw; }
void rtw88_unregister_hw(struct ieee80211_hw *hw)
{
    if (g_rtw88_hw == hw)
        g_rtw88_hw = NULL;
}

/* Accessor with external linkage so C++ TUs can retrieve the pointer without
 * declaring 'extern struct ieee80211_hw *g_rtw88_hw' — which would be UB
 * because the variable has internal (static) linkage. */
struct ieee80211_hw *rtw88_get_hw(void) { return g_rtw88_hw; }

struct ieee80211_hw *wiphy_to_ieee80211_hw(struct wiphy *wiphy)
{
    /* Return the actual allocation, never reinterpret a different structure. */
    return g_rtw88_hw && g_rtw88_hw->wiphy == wiphy ? g_rtw88_hw : NULL;
}

int cfg80211_get_ies_channel_number(const u8 *ie, size_t ielen,
                                     enum nl80211_band band)
{ (void)ie; (void)ielen; (void)band; return -1; }

bool cfg80211_ssid_eq(struct cfg80211_ssid *a, struct cfg80211_ssid *b)
{
    if (!a || !b) return false;
    if (a->ssid_len != b->ssid_len) return false;
    return memcmp(a->ssid, b->ssid, a->ssid_len) == 0;
}

int regulatory_hint(struct wiphy *wiphy, const char *alpha2)
{ (void)wiphy; (void)alpha2; return 0; }
#endif /* !RTW89_MACOS */

/* sdio_align_size stub — not needed for PCIe-only build */
unsigned int sdio_align_size(struct sdio_func *func, unsigned int size)
{ (void)func; return size; }

/* firmware loading is in rtw88_firmware.c (separate TU, no compat header conflicts) */

void *devm_kmemdup(struct device *dev, const void *src, size_t len, gfp_t gfp)
{
    (void)dev;
    void *p = kmalloc(len, gfp);
    if (p) memcpy(p, src, len);
    return p;
}

void *devm_kmemdup_array(struct device *dev, const void *src, size_t n,
                          size_t size, gfp_t gfp)
{
    (void)dev;
    void *p = kmalloc(n * size, gfp);
    if (p && src) memcpy(p, src, n * size);
    return p;
}

// removed devm_free_irq to avoid redefinition

/* ------------------------------------------------------------------ */
/*  Network device stubs                                                */
/* ------------------------------------------------------------------ */

struct net_device *alloc_netdev_dummy(int sizeof_priv)
{
    return (struct net_device *)kzalloc(
        sizeof(struct net_device) + sizeof_priv, GFP_KERNEL);
}

/* ------------------------------------------------------------------ */
/*  Misc kernel helpers                                                  */
/* ------------------------------------------------------------------ */

#ifndef RTW89_MACOS /* static inline in the upstream net/cfg80211.h */
void get_random_mask_addr(u8 *buf, const u8 *addr, const u8 *mask)
{
    u8 rand[6];
    read_random(rand, sizeof(rand));
    for (int i = 0; i < 6; i++)
        buf[i] = (addr[i] & ~mask[i]) | (rand[i] & mask[i]);
}
#endif /* !RTW89_MACOS */

int atomic_dec_if_positive(atomic_t *v)
{
    int c, old;
    c = atomic_read(v);
    for (;;) {
        if (c <= 0) return c - 1;
        old = __sync_val_compare_and_swap(&v->counter, c, c - 1);
        if (old == c) return c - 1;
        c = old;
    }
}

int timer_delete_sync(struct timer_list *timer)
{
    return del_timer_sync(timer);
}

/* ------------------------------------------------------------------ */
/*  Init / exit                                                         */
/* ------------------------------------------------------------------ */

int rtw88_compat_init(void)
{
    if (system_wq || system_long_wq || g_datapath_wq || rtw88_log_lock)
        return -EBUSY;

    work_state_lock = IOLockAlloc();
    if (!work_state_lock) return -ENOMEM;
    rtw88_log_lock = IOSimpleLockAlloc();
    system_wq      = alloc_workqueue("rtw88_system_wq", 0, 0);
    system_long_wq = alloc_workqueue("rtw88_long_wq",   0, 0);
    g_datapath_wq  = alloc_ordered_workqueue("rtw88_datapath_wq", 0);
    if (!system_wq || !system_long_wq || !g_datapath_wq || !rtw88_log_lock) {
        destroy_workqueue(g_datapath_wq);
        destroy_workqueue(system_wq);
        destroy_workqueue(system_long_wq);
        g_datapath_wq = NULL;
        system_wq = system_long_wq = NULL;
        if (rtw88_log_lock) {
            IOSimpleLockFree(rtw88_log_lock);
            rtw88_log_lock = NULL;
        }
        IOLockFree(work_state_lock);
        work_state_lock = NULL;
        return -ENOMEM;
    }

    return 0;
}

void rtw88_compat_exit(void)
{
    if (g_irq_work_initialized)
        cancel_work_sync(&g_irq_work);
    destroy_workqueue(g_datapath_wq);
    g_datapath_wq = NULL;
    destroy_workqueue(system_wq);
    destroy_workqueue(system_long_wq);
    system_wq = system_long_wq = NULL;
    if (work_state_lock) { IOLockFree(work_state_lock); work_state_lock = NULL; }
    g_irq_handler   = NULL;
    g_irq_thread_fn = NULL;
    g_irq_dev_id    = NULL;
    g_irq_work_initialized = false;
    g_tx_resume_cb  = NULL;
#ifndef RTW89_MACOS
    g_hw_cbs        = NULL;
    g_kext_hw       = NULL;
    g_rtw88_vif     = NULL;
#endif
    if (rtw88_log_lock) {
        IOSimpleLockFree(rtw88_log_lock);
        rtw88_log_lock = NULL;
    }
}

#ifndef RTW89_MACOS /* everything below reaches into struct rtw_dev (rtw88) */
/* ------------------------------------------------------------------ */
/*  Driver Info Helpers                                                 */
/* ------------------------------------------------------------------ */

#include "main.h"
#include "fw.h"
#include "reg.h"

/* We can't include rtw88's pci.h here because the compat tree's
 * linux/pci.h shadows it on the include path.  Just hardcode the few
 * register offsets and bits we need; they're stable in rtw88. */
#define RTW88_DBG_RTK_PCI_HIMR0        0x0B0
#define RTW88_DBG_RTK_PCI_HISR0        0x0B4
#define RTW88_DBG_RTK_PCI_HIMR1        0x0B8
#define RTW88_DBG_RTK_PCI_HISR1        0x0BC
#define RTW88_DBG_RTK_PCI_HISR3        0x10BC  /* 3081-wcpu (8822C) only */
#define RTW88_DBG_RTK_PCI_TXBD_IDX_BEQ 0x3A8
#define RTW88_DBG_REG_TXPAUSE          0x0522  /* MAC-level AC queue pause */
#define RTW88_DBG_REG_FWHW_TXQ_CTRL    0x0420  /* TXQ control / enable */
#define RTW88_DBG_REG_TCR              0x0604  /* TX Configuration Reg */
#define RTW88_DBG_REG_TXDMA_STATUS     0x0210  /* TX DMA status; bit2=PAGE_OVF */
#define RTW88_DBG_REG_TXPKT_EMPTY      0x041A  /* per-queue TX FIFO empty bits */
#define RTW88_DBG_REG_RXBD_IDX_MPDUQ   0x03B4  /* RX BD idx: hi16=hw_wp lo16=rp */

/* Defined in pci.c — dumps the BE buffer descriptor at a ring slot. */
extern void rtw88_get_be_bd(struct rtw_dev *rtwdev, u32 idx,
                            u32 *dma0, u32 *dma1,
                            u16 *bufsz0, u16 *bufsz1, u16 *psb0);
#define RTW88_DBG_TRX_BD_IDX_MASK      0xFFF
#define RTW88_DBG_IMR_BEDOK            (1u << 4)

struct rtw_pci;
extern void rtw_pci_enable_interrupt(struct rtw_dev *rtwdev, struct rtw_pci *rtwpci, bool exclude_rx);

void rtw88_reenable_interrupt(void)
{
    if (g_irq_dev_id) {
        struct rtw_dev *rtwdev = (struct rtw_dev *)g_irq_dev_id;
        struct rtw_pci *rtwpci = (struct rtw_pci *)rtwdev->priv;
        rtw_pci_enable_interrupt(rtwdev, rtwpci, false);
    }
}

extern void rtw88_get_be_ring_state(struct rtw_dev *rtwdev,
    u32 *sw_wp, u32 *sw_rp, u32 *qlen);

/*
 * Dump current chip-side TX state for the BE queue.  Used by the kext's
 * periodic debug timer to distinguish three failure modes when TX appears
 * stuck.  Now includes SW ring state for cross-checking:
 *   1) chip stopped: hw_rp frozen, sw_rp == hw_rp, qlen > 0
 *   2) chip moves, no IRQ:  hw_rp increasing but sw_rp lags
 *   3) IRQ pending but masked: BEDOK_pending=1 AND BEDOK_masked=1
 *   4) ring desync: hw_rp > hw_wp (chip consumed more than submitted)
 */
void rtw88_debug_dump_tx_state(void)
{
    if (!g_irq_dev_id) return;
    struct rtw_dev *rtwdev = (struct rtw_dev *)g_irq_dev_id;

    u32 himr0    = rtw_read32(rtwdev, RTW88_DBG_RTK_PCI_HIMR0);
    u32 hisr0    = rtw_read32(rtwdev, RTW88_DBG_RTK_PCI_HISR0);
    u32 himr1    = rtw_read32(rtwdev, RTW88_DBG_RTK_PCI_HIMR1);
    u32 hisr1    = rtw_read32(rtwdev, RTW88_DBG_RTK_PCI_HISR1);
    u32 hisr3    = rtw_read32(rtwdev, RTW88_DBG_RTK_PCI_HISR3);
    u32 bd_idx   = rtw_read32(rtwdev, RTW88_DBG_RTK_PCI_TXBD_IDX_BEQ);
    /* REG_TXPAUSE is 1 byte at 0x0522 — UNALIGNED for 32-bit read.
     * Must use rtw_read8 or we get neighbouring-register noise. */
    u8  txpause  = rtw_read8(rtwdev, RTW88_DBG_REG_TXPAUSE);
    u32 txqctrl  = rtw_read32(rtwdev, RTW88_DBG_REG_FWHW_TXQ_CTRL);
    u32 tcr      = rtw_read32(rtwdev, RTW88_DBG_REG_TCR);
    /* Distinguish "on-air TX stopped, FIFO full" from "DMA can't ingest":
     *   txdma_st bit2 (PAGE_OVF) set or pkt_empty BE-bit clear => frames are
     *   buffered in the chip FIFO and not radiating (FIFO backed up).
     *   pkt_empty BE-bit set while rp stuck => FIFO drained, chip still won't
     *   ingest the descriptor at rp => descriptor/DMA-read stall. */
    u32 txdma_st = rtw_read32(rtwdev, RTW88_DBG_REG_TXDMA_STATUS);
    u16 pkt_empty= rtw_read16(rtwdev, RTW88_DBG_REG_TXPKT_EMPTY);
    u32 hw_wp    = bd_idx & RTW88_DBG_TRX_BD_IDX_MASK;
    u32 hw_rp    = (bd_idx >> 16) & RTW88_DBG_TRX_BD_IDX_MASK;

    /* SW ring state from pci.c (can't include pci.h — path shadowing) */
    u32 sw_wp = 0, sw_rp = 0, qlen = 0;
    rtw88_get_be_ring_state(rtwdev, &sw_wp, &sw_rp, &qlen);

    /* Dump the BE buffer descriptor the HW read pointer is parked on.
     * If dma0/dma1 are zero or insane, the chip is stalled on a bad
     * descriptor (our bounce-buffer bug); if they look valid the chip
     * stopped fetching for another reason. */
    u32 bd_dma0 = 0, bd_dma1 = 0; u16 bd_sz0 = 0, bd_sz1 = 0, bd_psb = 0;
    rtw88_get_be_bd(rtwdev, hw_rp, &bd_dma0, &bd_dma1, &bd_sz0, &bd_sz1, &bd_psb);

    /* RX BD index register: hi16 = HW write ptr, lo16 = read ptr. Lets us see
     * whether RX DMA also froze (chip-wide stall) or only TX. */
    u32 rxbd = rtw_read32(rtwdev, RTW88_DBG_REG_RXBD_IDX_MPDUQ);
    u32 rx_hw_wp = (rxbd >> 16) & RTW88_DBG_TRX_BD_IDX_MASK;
    u32 rx_rp    = rxbd & RTW88_DBG_TRX_BD_IDX_MASK;

    IOLog("rtw88: TXSTATE BE hw_wp=%u hw_rp=%u sw_wp=%u sw_rp=%u qlen=%u "
          "TXDMA_ST=0x%08x PKT_EMPTY=0x%04x BD[rp]:dma0=0x%08x dma1=0x%08x "
          "sz0=%u sz1=%u psb=0x%04x RX_rp=%u RX_hwwp=%u "
          "HISR0=0x%08x HISR3=0x%08x BEDOK_p=%d\n",
          hw_wp, hw_rp, sw_wp, sw_rp, qlen,
          txdma_st, pkt_empty, bd_dma0, bd_dma1, bd_sz0, bd_sz1, bd_psb,
          rx_rp, rx_hw_wp,
          hisr0, hisr3,
          (hisr0 & RTW88_DBG_IMR_BEDOK) ? 1 : 0);
}

bool rtw88_is_scanning(void)
{
    if (!g_rtw88_hw || !g_rtw88_hw->priv) return false;
    struct rtw_dev *rtwdev = (struct rtw_dev *)g_rtw88_hw->priv;
    return test_bit(RTW_FLAG_SCANNING, rtwdev->flags);
}

bool rtw88_hw_scan_supported(struct ieee80211_hw *hw)
{
    if (!hw || !hw->priv) return false;
    struct rtw_dev *rtwdev = (struct rtw_dev *)hw->priv;
    /* The macOS caller does not populate the probe SSIDs/IEs required by
     * this port's offload path. Use the existing software scanner on 8822C
     * instead of advertising firmware scan capability as usable support.
     * Keep other chips on their existing policy. */
    if (rtwdev->chip->id == RTW_CHIP_TYPE_8822C)
        return false;
    return rtw_fw_feature_check(&rtwdev->fw, FW_FEATURE_SCAN_OFFLOAD);
}

void rtw88_sw_scan_start(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
    if (!hw || !hw->priv || !vif) return;
    struct rtw_dev *rtwdev = (struct rtw_dev *)hw->priv;
    struct rtw_vif *rtwvif = (struct rtw_vif *)vif->drv_priv;
    if (!rtwvif) return;

    mutex_lock(&rtwdev->mutex);
    rtw_core_scan_start(rtwdev, rtwvif, vif->addr, false);
    rtwdev->hal.rcr &= ~BIT_CBSSID_BCN;
    rtw_write32(rtwdev, REG_RCR, rtwdev->hal.rcr);
    mutex_unlock(&rtwdev->mutex);
}

/* core_start resets MAC registers. Like rtw_leave_ips, restore the existing
 * port after power-up; calling add_interface again would leak a port/mac_id. */
int rtw88_restore_interface(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
    if (!hw || !hw->priv || !vif) return -EINVAL;
    struct rtw_dev *rtwdev = (struct rtw_dev *)hw->priv;
    struct rtw_vif *rtwvif = (struct rtw_vif *)vif->drv_priv;
    if (!rtwvif->conf) return -EINVAL;
    mutex_lock(&rtwdev->mutex);
    rtw_vif_port_config(rtwdev, rtwvif, ~0U);
    if (hw->conf.chandef.chan) rtw_set_channel(rtwdev);
    rtwdev->need_rfk = true;
    mutex_unlock(&rtwdev->mutex);
    return 0;
}

int rtw88_set_station_mac(struct ieee80211_hw *hw,
                          struct ieee80211_vif *vif,
                          const uint8_t *mac)
{
    if (!hw || !hw->priv || !vif || !mac)
        return -EINVAL;

    struct rtw_dev *rtwdev = (struct rtw_dev *)hw->priv;
    struct rtw_vif *rtwvif = (struct rtw_vif *)vif->drv_priv;
    if (!rtwvif)
        return -EINVAL;

    memcpy(vif->addr, mac, ETH_ALEN);
    memcpy(rtwvif->mac_addr, mac, ETH_ALEN);

    mutex_lock(&rtwdev->mutex);
    rtw_vif_port_config(rtwdev, rtwvif, PORT_SET_MAC_ADDR);
    mutex_unlock(&rtwdev->mutex);
    return 0;
}

void rtw88_sw_scan_switch_channel(struct ieee80211_hw *hw)
{
    if (!hw || !hw->priv) return;
    struct rtw_dev *rtwdev = (struct rtw_dev *)hw->priv;

    mutex_lock(&rtwdev->mutex);
    rtw_set_channel(rtwdev);
    mutex_unlock(&rtwdev->mutex);
}

/* Dedicated AWDL channel switch for the unassociated case. Unlike the scan
 * helper, consume the RF-calibration request before transmitting data/action
 * frames on the new channel. */
void rtw88_awdl_switch_channel(struct ieee80211_hw *hw)
{
    if (!hw || !hw->priv) return;
    struct rtw_dev *rtwdev = (struct rtw_dev *)hw->priv;
    mutex_lock(&rtwdev->mutex);
    rtw_set_channel(rtwdev);
    rtwdev->need_rfk = true;
    rtw_chip_prepare_tx(rtwdev);
    mutex_unlock(&rtwdev->mutex);
}

/* Connected STA/AWDL coexistence must hop quickly enough to fit inside an
 * AWDL common availability window.  Full IQK/DPK/GAPK on every 10-16 ms
 * excursion would consume the window itself.  This mirrors the already
 * proven connected software-scan channel hop: program the PHY only and keep
 * the expensive RFK for normal association/channel setup. */
void rtw88_awdl_timeslice_switch_channel(struct ieee80211_hw *hw)
{
    if (!hw || !hw->priv) return;
    struct rtw_dev *rtwdev = (struct rtw_dev *)hw->priv;
    mutex_lock(&rtwdev->mutex);
    rtw_set_channel(rtwdev);
    mutex_unlock(&rtwdev->mutex);
}

void rtw88_sw_scan_complete(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
    if (!hw || !hw->priv || !vif) return;
    struct rtw_dev *rtwdev = (struct rtw_dev *)hw->priv;

    mutex_lock(&rtwdev->mutex);
    rtwdev->hal.rcr |= BIT_CBSSID_BCN;
    rtw_write32(rtwdev, REG_RCR, rtwdev->hal.rcr);
    rtw_core_scan_complete(rtwdev, vif, false);
    mutex_unlock(&rtwdev->mutex);
}

/*
 * Force-disable Bluetooth coexistence by clearing efuse.btcoex.
 *
 * rtw_power_on (called from rtw_core_start) reads efuse.btcoex to decide
 * wifi_only.  When wifi_only=false the coex driver runs periodic H2C
 * queries asking the firmware to track BT activity; on a hackintosh with
 * no functional BT controller the firmware never gets a clean "BT idle"
 * reply and can leave the WiFi TX engine starved — chip stops fetching
 * BE descriptors silently, no IMR bit set, no TXPAUSE.
 *
 * Must be called AFTER rtw_pci_probe (which runs rtw_core_init to fill
 * the efuse struct) and BEFORE rtw_core_start.
 */
void rtw88_force_wifi_only(void)
{
    if (!g_rtw88_hw || !g_rtw88_hw->priv) return;
    struct rtw_dev *rtwdev = (struct rtw_dev *)g_rtw88_hw->priv;
    if (rtwdev->efuse.btcoex) {
        IOLog("rtw88: forcing wifi_only (was efuse.btcoex=1)\n");
        rtwdev->efuse.btcoex = 0;
    }
}

/*
 * rtw88_connect_hw_setup — set channel + BSSID for the connect flow.
 *
 * Called from doAuthenticate() INSTEAD of ops->config + ops->bss_info_changed.
 *
 * Why not use the ops callbacks:
 *   rtw_ops_config() calls rtw_leave_lps_deep() which calls
 *   __rtw_fw_leave_lps_check_reg() — a polling loop of MMIO *reads*
 *   (rtw_read32_mask on REG_TCR).  If the chip is unresponsive immediately
 *   post-HW-scan (firmware still in internal scan-exit critical section),
 *   those reads stall the CPU core indefinitely → PCIe timeout → system
 *   freeze.  The rtw_ops_bss_info_changed() path has the same issue.
 *
 * This helper holds rtwdev->mutex, calls rtw_set_channel() directly
 * (mostly MMIO *writes*, safe), then writes the BSSID register.
 * The caller must populate hw->conf.chandef before calling.
 */
void rtw88_connect_hw_setup(struct ieee80211_hw *hw,
                             struct ieee80211_vif *vif,
                             const uint8_t *bssid)
{
    if (!hw || !hw->priv || !vif || !bssid) return;
    struct rtw_dev *rtwdev = (struct rtw_dev *)hw->priv;
    struct rtw_vif *rtwvif = (struct rtw_vif *)vif->drv_priv;

    mutex_lock(&rtwdev->mutex);

    /* Switch to the AP's channel (uses hw->conf.chandef set by caller). */
    IOLog("rtw88: connect_hw_setup: rtw_set_channel\n");
    rtw_set_channel(rtwdev);
    IOLog("rtw88: connect_hw_setup: set_channel done\n");

    /* Write target BSSID to port-0 hardware register. */
    if (rtwvif) {
        memcpy(rtwvif->bssid, bssid, ETH_ALEN);
        rtw_vif_port_config(rtwdev, rtwvif, PORT_SET_BSSID);
        IOLog("rtw88: connect_hw_setup: BSSID written\n");
    }

    /*
     * Run RF calibration (IQK / DPK / GAPK) for the just-selected channel.
     *
     * The stock driver performs this in ieee80211_ops::mgd_prepare_tx() ->
     * rtw_chip_prepare_tx(), guarded by rtwdev->need_rfk (which rtw_set_channel
     * above just set).  This port bypasses mac80211, so mgd_prepare_tx() is
     * never invoked and the calibration never ran — leaving the 8822C TX path
     * uncalibrated.  Uncalibrated TX manages a handful of frames, then the
     * signal degrades enough that the AP stops ACKing; the chip retries to
     * exhaustion, on-air throughput collapses, the TX FIFO backs up and the
     * BE ring's HW read pointer stalls (taking RX down with it on the shared
     * MAC).  Consume need_rfk here so calibration actually happens before data
     * frames flow.  We already hold rtwdev->mutex, matching rtw_ops_start_ap's
     * calibration call site.
     */
    rtwdev->need_rfk = true;
    IOLog("rtw88: connect_hw_setup: running RF calibration (IQK/DPK/GAPK)\n");
    rtw_chip_prepare_tx(rtwdev);
    IOLog("rtw88: connect_hw_setup: RF calibration done\n");

    /*
     * Let the firmware rate-adaptation pick the data rate on both bands.
     *
     * The old 5GHz "pin to 6M OFDM" crutch capped 5GHz at ~5 Mbps. It existed
     * only because the association was legacy-only, so the RA mask held nothing
     * but legacy rates and the firmware sometimes settled on a rate the AP
     * wouldn't ACK. Now that we advertise HT/VHT and mirror those caps onto the
     * STA (rtw_update_sta_info builds an HT/VHT RA mask), the firmware adapts
     * across the full MCS set — pinning would only hurt. fix_rate = 0xFF
     * (U8_MAX) leaves use_rate false so the firmware RA drives the rate.
     */
    rtwdev->dm_info.fix_rate = 0xFF;

    mutex_unlock(&rtwdev->mutex);
}

void rtw88_restore_connected_hw(struct ieee80211_hw *hw,
                                 struct ieee80211_vif *vif,
                                 const uint8_t *bssid)
{
    if (!hw || !hw->priv || !vif || !bssid) return;
    struct rtw_dev *rtwdev = (struct rtw_dev *)hw->priv;
    struct rtw_vif *rtwvif = (struct rtw_vif *)vif->drv_priv;

    mutex_lock(&rtwdev->mutex);

    rtw_set_channel(rtwdev);

    if (rtwvif) {
        memcpy(rtwvif->bssid, bssid, ETH_ALEN);
        rtw_vif_port_config(rtwdev, rtwvif, PORT_SET_BSSID);
    }

    /* rtw_set_channel deliberately defers calibration during a scan.
     * Home-channel visits must obey that rule too: forcing IQK here can
     * block for seconds on every visit while the AP buffers our traffic.
     * Calibrate once after scan completion clears RTW_FLAG_SCANNING. */
    if (!test_bit(RTW_FLAG_SCANNING, rtwdev->flags))
        rtw_chip_prepare_tx(rtwdev);

    /* Firmware rate-adaptation on both bands (see rtw88_connect_hw_setup). */
    rtwdev->dm_info.fix_rate = 0xFF;

    mutex_unlock(&rtwdev->mutex);
}

/* Fast home-channel restore used between AWDL availability windows.  The AP
 * was put into power-save buffering before the hop; restore channel/BSSID and
 * firmware RA without forcing a multi-millisecond RF calibration every EAW.
 * The normal connect/recovery paths still run full RFK. */
void rtw88_restore_connected_hw_timeslice(struct ieee80211_hw *hw,
                                          struct ieee80211_vif *vif,
                                          const uint8_t *bssid)
{
    if (!hw || !hw->priv || !vif || !bssid) return;
    struct rtw_dev *rtwdev = (struct rtw_dev *)hw->priv;
    struct rtw_vif *rtwvif = (struct rtw_vif *)vif->drv_priv;

    mutex_lock(&rtwdev->mutex);
    rtw_set_channel(rtwdev);
    if (rtwvif) {
        memcpy(rtwvif->bssid, bssid, ETH_ALEN);
        rtw_vif_port_config(rtwdev, rtwvif, PORT_SET_BSSID);
    }
    rtwdev->dm_info.fix_rate = 0xFF;
    mutex_unlock(&rtwdev->mutex);
}

void rtw88_get_fw_version(struct rtw_dev *rtwdev, uint16_t *version, uint8_t *sub_version)
{
    if (version) *version = 0;
    if (sub_version) *sub_version = 0;
    if (!rtwdev) return;
    if (version) *version = rtwdev->fw.version;
    if (sub_version) *sub_version = rtwdev->fw.sub_version;
}

void rtw88_get_chip_name(struct rtw_dev *rtwdev, char *name_buf, size_t buf_sz)
{
    if (!name_buf || buf_sz == 0) return;
    name_buf[0] = '\0';
    if (!rtwdev || !rtwdev->chip) {
        strlcpy(name_buf, "Unknown", buf_sz);
        return;
    }
    switch (rtwdev->chip->id) {
    case RTW_CHIP_TYPE_8822B: strlcpy(name_buf, "RTL8822BE", buf_sz); break;
    case RTW_CHIP_TYPE_8822C: strlcpy(name_buf, "RTL8822CE", buf_sz); break;
    case RTW_CHIP_TYPE_8723D: strlcpy(name_buf, "RTL8723DE", buf_sz); break;
    case RTW_CHIP_TYPE_8821C: strlcpy(name_buf, "RTL8821CE", buf_sz); break;
    case RTW_CHIP_TYPE_8703B: strlcpy(name_buf, "RTL8703BE", buf_sz); break;
    default: strlcpy(name_buf, "RTL88xx (Unknown)", buf_sz); break;
    }
}

void rtw88_get_stats(struct rtw_dev *rtwdev, uint32_t *tx_bytes, uint32_t *rx_bytes)
{
    if (tx_bytes) *tx_bytes = 0;
    if (rx_bytes) *rx_bytes = 0;
    if (!rtwdev) return;
    if (tx_bytes) *tx_bytes = (uint32_t)rtwdev->stats.tx_unicast;
    if (rx_bytes) *rx_bytes = (uint32_t)rtwdev->stats.rx_unicast;
}

uint8_t rtw88_get_tx_nss(struct rtw_dev *rtwdev)
{
    if (!rtwdev) return 1;

    /* main.c normalises efuse.hw_cap.nss against rf_path_num during
     * capability setup, but keep the fallback here as defensive ABI glue. */
    uint8_t nss = rtwdev->efuse.hw_cap.nss;
    if (nss == 0 || nss > 4)
        nss = rtwdev->hal.rf_path_num;
    if (nss == 0 || nss > 4)
        nss = 1;
    return nss;
}

uint32_t rtw88_read_log(char *out_buf, uint32_t max_len)
{
    if (!out_buf || max_len == 0 || !rtw88_log_lock) return 0;
    
    uint32_t read = 0;
    IOSimpleLockLock(rtw88_log_lock);
    while (rtw88_log_tail != rtw88_log_head && read < max_len) {
        out_buf[read++] = rtw88_log_ring[rtw88_log_tail];
        rtw88_log_tail = (rtw88_log_tail + 1) % sizeof(rtw88_log_ring);
    }
    IOSimpleLockUnlock(rtw88_log_lock);
    return read;
}
#endif /* !RTW89_MACOS */
