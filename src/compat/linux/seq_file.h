/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_SEQ_FILE_H
#define _RTW88_COMPAT_SEQ_FILE_H

#include "types.h"

/* Minimal seq_file stub for debugfs-like output */
struct seq_file {
    char  *buf;
    size_t size;
    size_t count;
};

static inline int seq_printf(struct seq_file *m, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static inline int seq_printf(struct seq_file *m, const char *fmt, ...)
{
    (void)m; (void)fmt; return 0;
}

static inline void seq_puts(struct seq_file *m, const char *s) { (void)m; (void)s; }
static inline void seq_putc(struct seq_file *m, char c) { (void)m; (void)c; }

#endif /* _RTW88_COMPAT_SEQ_FILE_H */
