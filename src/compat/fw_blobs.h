/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/* Firmware images embedded in the kext; the table is generated from
 * firmware/ by tools/gen_fw_blobs.py and ends with a NULL name. */
#ifndef _RTW88_FW_BLOBS_H
#define _RTW88_FW_BLOBS_H

#include <stddef.h>
#include <stdint.h>

struct rtw88_fw_blob {
    const char    *name;              /* file name without directory */
    const uint8_t *data;              /* zlib stream */
    size_t         compressed_size;
    size_t         uncompressed_size;
};

extern const struct rtw88_fw_blob rtw88_fw_blobs[];

#endif /* _RTW88_FW_BLOBS_H */
