// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* Runtime pieces of the rtw89 compat additions (see rtw89_compat.h). */

#include "rtw89_compat.h"
#include <kern/clock.h>
#include <sys/random.h>

/* ------------------------------------------------------------------ */
/*  Time / random                                                       */
/* ------------------------------------------------------------------ */

u64 rtw89_compat_ktime_ns(void)
{
    u64 ns;

    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
}

u32 rtw89_compat_random_u32(void)
{
    u32 v;

    read_random(&v, sizeof(v));
    return v;
}

/* ------------------------------------------------------------------ */
/*  wiphy_work                                                          */
/* ------------------------------------------------------------------ */

static void rtw89_compat_wiphy_runner(struct work_struct *w)
{
    struct wiphy *wiphy = container_of(w, struct wiphy, wiphy_work_runner);
    struct wiphy_work *work;
    unsigned long flags;

    wiphy_lock(wiphy);

    spin_lock_irqsave(&wiphy->wiphy_work_lock, flags);
    if (list_empty(&wiphy->wiphy_work_list)) {
        spin_unlock_irqrestore(&wiphy->wiphy_work_lock, flags);
        wiphy_unlock(wiphy);
        return;
    }
    work = list_first_entry(&wiphy->wiphy_work_list, struct wiphy_work, entry);
    list_del_init(&work->entry);
    /* One work per pass, like cfg80211, so other lock waiters get a turn. */
    if (!list_empty(&wiphy->wiphy_work_list))
        schedule_work(&wiphy->wiphy_work_runner);
    spin_unlock_irqrestore(&wiphy->wiphy_work_lock, flags);

    work->func(wiphy, work);

    wiphy_unlock(wiphy);
}

void rtw89_compat_wiphy_init(struct wiphy *wiphy)
{
    mutex_init(&wiphy->mtx);
    spin_lock_init(&wiphy->wiphy_work_lock);
    INIT_LIST_HEAD(&wiphy->wiphy_work_list);
    INIT_WORK(&wiphy->wiphy_work_runner, rtw89_compat_wiphy_runner);
}

void rtw89_compat_wiphy_exit(struct wiphy *wiphy)
{
    unsigned long flags;

    cancel_work_sync(&wiphy->wiphy_work_runner);
    spin_lock_irqsave(&wiphy->wiphy_work_lock, flags);
    while (!list_empty(&wiphy->wiphy_work_list))
        list_del_init(wiphy->wiphy_work_list.next);
    spin_unlock_irqrestore(&wiphy->wiphy_work_lock, flags);
    mutex_destroy(&wiphy->mtx);
}

void wiphy_work_queue(struct wiphy *wiphy, struct wiphy_work *work)
{
    unsigned long flags;

    spin_lock_irqsave(&wiphy->wiphy_work_lock, flags);
    if (list_empty(&work->entry))
        list_add_tail(&work->entry, &wiphy->wiphy_work_list);
    spin_unlock_irqrestore(&wiphy->wiphy_work_lock, flags);

    schedule_work(&wiphy->wiphy_work_runner);
}

/* Caller holds the wiphy mutex, so @work is not running; just unlink it. */
void wiphy_work_cancel(struct wiphy *wiphy, struct wiphy_work *work)
{
    unsigned long flags;

    spin_lock_irqsave(&wiphy->wiphy_work_lock, flags);
    if (!list_empty(&work->entry))
        list_del_init(&work->entry);
    spin_unlock_irqrestore(&wiphy->wiphy_work_lock, flags);
}

/* Caller holds the wiphy mutex: run @work now if it is pending. */
void wiphy_work_flush(struct wiphy *wiphy, struct wiphy_work *work)
{
    unsigned long flags;
    bool run = false;

    spin_lock_irqsave(&wiphy->wiphy_work_lock, flags);
    if (!list_empty(&work->entry)) {
        list_del_init(&work->entry);
        run = true;
    }
    spin_unlock_irqrestore(&wiphy->wiphy_work_lock, flags);

    if (run)
        work->func(wiphy, work);
}

/* Timer stage of a delayed wiphy work: hand it to the runner. Never takes the mutex. */
void rtw89_compat_wiphy_delayed_work_timer(struct work_struct *w)
{
    struct delayed_work *dw = container_of(w, struct delayed_work, work);
    struct wiphy_delayed_work *dwork = container_of(dw, struct wiphy_delayed_work, dwork);

    wiphy_work_queue(dwork->wiphy, &dwork->work);
}

void wiphy_delayed_work_queue(struct wiphy *wiphy, struct wiphy_delayed_work *dwork,
                              unsigned long delay)
{
    if (!delay) {
        cancel_delayed_work(&dwork->dwork);
        wiphy_work_queue(wiphy, &dwork->work);
        return;
    }
    dwork->wiphy = wiphy;
    /* Re-arm semantics (Linux mod_delayed_work on system_unbound_wq). Must be the
     * system queue: queue_delayed_work(NULL, ...) is a silent no-op in the shim. */
    cancel_delayed_work(&dwork->dwork);
    schedule_delayed_work(&dwork->dwork, delay);
}

/*
 * The timer stage only queues (no mutex), so waiting for it here while holding
 * the wiphy mutex is safe.
 */
void wiphy_delayed_work_cancel(struct wiphy *wiphy, struct wiphy_delayed_work *dwork)
{
    cancel_delayed_work_sync(&dwork->dwork);
    wiphy_work_cancel(wiphy, &dwork->work);
}

void wiphy_delayed_work_flush(struct wiphy *wiphy, struct wiphy_delayed_work *dwork)
{
    bool was_armed = cancel_delayed_work_sync(&dwork->dwork);

    if (was_armed)
        wiphy_work_queue(wiphy, &dwork->work);
    wiphy_work_flush(wiphy, &dwork->work);
}
