/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_WORKQUEUE_H
#define _RTW88_COMPAT_WORKQUEUE_H

#include "types.h"
#include "spinlock.h"
#include "timer.h"
#include "../iokit_shim.h"

struct work_struct;
struct delayed_work;

typedef void (*work_func_t)(struct work_struct *work);

struct work_struct {
    work_func_t      func;
    struct list_head entry;
    unsigned long    pending; /* 1: queued, 2: delayed timer */
    struct workqueue_struct *wq;
    bool executing;
    bool canceling;
};

struct delayed_work {
    struct work_struct  work;
    struct timer_list   timer;  /* defined in timer.h */
};

struct workqueue_struct {
    thread_t         thread;
    IOLock          *lock;
    struct list_head queue;
    int              running;
    int              done;
    unsigned int     active;
    char             name[64];
};

#define INIT_WORK(_work, _func) \
    do { (_work)->func = (_func); \
         INIT_LIST_HEAD(&(_work)->entry); \
         (_work)->pending = 0; (_work)->wq = NULL; \
         (_work)->executing = false; (_work)->canceling = false; } while (0)

#define INIT_DELAYED_WORK(_dwork, _func) \
    do { INIT_WORK(&(_dwork)->work, _func); \
         timer_setup(&(_dwork)->timer, rtw88_delayed_work_timer_fn, 0); } while (0)

void rtw88_delayed_work_timer_fn(struct timer_list *t);

extern struct workqueue_struct *system_wq;
extern struct workqueue_struct *system_long_wq;

struct workqueue_struct *alloc_workqueue(const char *name, unsigned int flags,
                                          int max_active);
struct workqueue_struct *alloc_ordered_workqueue(const char *name,
                                                  unsigned int flags);
void destroy_workqueue(struct workqueue_struct *wq);

bool queue_work(struct workqueue_struct *wq, struct work_struct *work);
bool queue_delayed_work(struct workqueue_struct *wq,
                        struct delayed_work *dwork, unsigned long delay);
void flush_workqueue(struct workqueue_struct *wq);
bool cancel_work_sync(struct work_struct *work);
bool cancel_delayed_work_sync(struct delayed_work *dwork);
bool cancel_delayed_work(struct delayed_work *dwork);
void flush_work(struct work_struct *work);
bool schedule_work(struct work_struct *work);
bool schedule_delayed_work(struct delayed_work *dwork, unsigned long delay);
void flush_scheduled_work(void);

#define WQ_HIGHPRI     0
#define WQ_UNBOUND     0
#define WQ_MEM_RECLAIM 0
#define WQ_FREEZABLE   0
#define WQ_BH          0
#define WQ_PERCPU      0

static inline bool queue_work_on(int cpu, struct workqueue_struct *wq,
                                  struct work_struct *work)
{
    return queue_work(wq, work);
}

static inline bool mod_delayed_work(struct workqueue_struct *wq,
                                     struct delayed_work *dwork,
                                     unsigned long delay)
{
    cancel_delayed_work(dwork);
    return queue_delayed_work(wq, dwork, delay);
}

int  rtw88_workqueue_init(void);
void rtw88_workqueue_exit(void);

struct workqueue_struct *create_singlethread_workqueue(const char *);

#endif /* _RTW88_COMPAT_WORKQUEUE_H */
