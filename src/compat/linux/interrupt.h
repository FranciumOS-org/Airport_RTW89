/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_INTERRUPT_H
#define _RTW88_COMPAT_INTERRUPT_H

#include "types.h"
#include "workqueue.h"

#define IRQF_SHARED     0x00000080
#define IRQF_DISABLED   0x00000020

typedef int irqreturn_t;
#define IRQ_NONE        0
#define IRQ_HANDLED     1
#define IRQ_WAKE_THREAD 2

typedef irqreturn_t (*irq_handler_t)(int, void *);

#ifndef KBUILD_MODNAME
#define KBUILD_MODNAME "rtw88"
#endif

/* In the kext, interrupts are registered via IOInterruptEventSource.
 * These stubs satisfy compilation of driver C files.
 */
struct device;

static inline int request_irq(unsigned int irq,
                               irqreturn_t (*handler)(int, void *),
                               unsigned long flags,
                               const char *name, void *dev)
{
    /* Handled by kext IOInterruptEventSource */
    return 0;
}

static inline void free_irq(unsigned int irq, void *dev_id) {}

void rtw88_devm_free_irq(struct device *dev, unsigned int irq, void *dev_id);
#define devm_free_irq rtw88_devm_free_irq

static inline void enable_irq(unsigned int irq) {}
static inline void disable_irq(unsigned int irq) {}
static inline void disable_irq_nosync(unsigned int irq) {}
/* rtw_pci_stop() must wait for an already queued threaded bottom half before
 * NAPI and DMA rings are destroyed (sleep/disable included). */
void rtw88_synchronize_irq(void);
static inline void synchronize_irq(unsigned int irq)
{
    (void)irq;
    rtw88_synchronize_irq();
}

/* tasklet — simplified to direct call in our context */
struct tasklet_struct {
    void (*func)(unsigned long);
    unsigned long data;
};

static inline void tasklet_init(struct tasklet_struct *t,
                                 void (*func)(unsigned long),
                                 unsigned long data)
{
    t->func = func;
    t->data = data;
}

static inline void tasklet_schedule(struct tasklet_struct *t)
{
    if (t->func) t->func(t->data);
}

static inline void tasklet_kill(struct tasklet_struct *t) {}

/* NAPI — not used directly; kext handles RX scheduling */
struct net_device;
struct napi_struct {
    struct net_device *dev;
    int (*poll)(struct napi_struct *, int);
    int weight;
    bool enabled;
    struct work_struct work;
};

void rtw88_netif_napi_add(struct net_device *dev, struct napi_struct *napi, int (*poll_fn)(struct napi_struct *, int));
#define netif_napi_add rtw88_netif_napi_add

void rtw88_napi_enable(struct napi_struct *napi);
void rtw88_napi_disable(struct napi_struct *napi);
void rtw88_napi_synchronize(struct napi_struct *napi);
#define napi_enable rtw88_napi_enable
#define napi_disable rtw88_napi_disable
#define napi_synchronize rtw88_napi_synchronize

void rtw88_napi_schedule(struct napi_struct *napi);
#define napi_schedule rtw88_napi_schedule

static inline void napi_complete(struct napi_struct *napi) { (void)napi; }
static inline int napi_reschedule(struct napi_struct *napi)
{
    rtw88_napi_schedule(napi);
    return 1;
}

void rtw88_netif_napi_del(struct napi_struct *napi);
#define netif_napi_del rtw88_netif_napi_del

static inline int napi_complete_done(struct napi_struct *napi, int work)
{
    (void)napi; (void)work;
    return 1;
}

extern irq_handler_t g_irq_handler;
extern irq_handler_t g_irq_thread_fn;
extern void *g_irq_dev_id;

int rtw88_devm_request_threaded_irq(struct device *dev, unsigned int irq,
        irq_handler_t handler, irq_handler_t thread_fn,
        unsigned long flags, const char *name, void *dev_id);
#define devm_request_threaded_irq rtw88_devm_request_threaded_irq

#endif /* _RTW88_COMPAT_INTERRUPT_H */
