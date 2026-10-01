/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef RTW88_THREAD_CALL_H
#define RTW88_THREAD_CALL_H
#include <kern/thread_call.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Opaque handles owned exclusively by this shim. Never mix native and wrapped
 * calls. Producers must be stopped before freeing a handle, as in the XNU API. */
thread_call_t rtw88_thread_call_allocate(thread_call_func_t, thread_call_param_t);
boolean_t rtw88_thread_call_enter(thread_call_t);
boolean_t rtw88_thread_call_enter_delayed(thread_call_t, uint64_t);
boolean_t rtw88_thread_call_cancel(thread_call_t);
boolean_t rtw88_thread_call_cancel_wait(thread_call_t);
boolean_t rtw88_thread_call_free(thread_call_t);
#ifdef __cplusplus
}
#endif
#define thread_call_allocate rtw88_thread_call_allocate
#define thread_call_enter rtw88_thread_call_enter
#define thread_call_enter_delayed rtw88_thread_call_enter_delayed
#define thread_call_cancel rtw88_thread_call_cancel
#define thread_call_cancel_wait rtw88_thread_call_cancel_wait
#define thread_call_free rtw88_thread_call_free
#endif
