/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * rtw88_firmware.c — firmware loading for rtw88 macOS port.
 *
 * Firmware blobs are zlib-compressed C arrays embedded in the binary
 * (fw_blobs.c, generated from firmware/*.bin).  No VFS needed at boot.
 *
 * Compiled WITHOUT Linux compat headers to avoid type conflicts.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <IOKit/IOLib.h>
#include <libkern/zlib.h>

#include "fw_blobs.h"

/*
 * Must match linux/firmware.h field order (size then data).
 * _alloc_size is a private trailer used only by release_firmware().
 * Driver code only touches size and data, so the extra field is safe.
 */
struct firmware {
    size_t        size;
    const uint8_t *data;
    size_t        _alloc_size;  /* bytes passed to IOMalloc for data */
};

/* ------------------------------------------------------------------ */
/*  zlib allocators — mirrors itlwm/itl80211/zutil.c exactly          */
/* ------------------------------------------------------------------ */

typedef struct z_mem {
    uint32_t alloc_size;
    uint8_t  data[0];
} z_mem;

static voidpf rtw88_zcalloc(voidpf opaque, uInt items, uInt size)
{
    (void)opaque;
    uint32_t alloc = (uint32_t)(items * size) + (uint32_t)sizeof(z_mem *);
    z_mem *zm = (z_mem *)IOMalloc(alloc);
    if (!zm) return Z_NULL;
    zm->alloc_size = alloc;
    return (voidpf)zm->data;
}

static void rtw88_zcfree(voidpf opaque, voidpf ptr)
{
    (void)opaque;
    if (!ptr) return;
    z_mem *zm = (z_mem *)((uint32_t *)ptr - 1);
    IOFree(zm, zm->alloc_size);
}

/* ------------------------------------------------------------------ */
/*  Decompress one blob — mirrors itlwm's uncompressFirmware()        */
/* ------------------------------------------------------------------ */

static int rtw88_decompress(uint8_t *dest, uint32_t *dest_len,
                             const uint8_t *src, uint32_t src_len)
{
    z_stream zs;
    zs.next_in   = (Bytef *)src;
    zs.avail_in  = src_len;
    zs.next_out  = (Bytef *)dest;
    zs.avail_out = *dest_len;
    zs.zalloc    = rtw88_zcalloc;
    zs.zfree     = rtw88_zcfree;
    zs.opaque    = Z_NULL;

    int err = inflateInit(&zs);
    if (err != Z_OK) {
        IOLog("rtw88: inflateInit error %d\n", err);
        return err;
    }

    err = inflate(&zs, Z_FINISH);
    if (err != Z_STREAM_END) {
        IOLog("rtw88: inflate error %d (avail_in=%u avail_out=%u)\n",
              err, zs.avail_in, zs.avail_out);
        inflateEnd(&zs);
        return err;
    }

    *dest_len = (uint32_t)zs.total_out;
    err = inflateEnd(&zs);
    return (err == Z_OK) ? 0 : err;
}

/* ------------------------------------------------------------------ */
/*  Blob lookup + decompress                                           */
/* ------------------------------------------------------------------ */

static struct firmware *load_fw_from_blob(const char *base)
{
    for (int i = 0; rtw88_fw_blobs[i].name; i++) {
        if (strcmp(rtw88_fw_blobs[i].name, base) != 0)
            continue;

        const struct rtw88_fw_blob *b = &rtw88_fw_blobs[i];

        /* 4× compressed size — same generous bound as itlwm */
        size_t alloc = b->compressed_size * 4;
        uint32_t out_len = (uint32_t)alloc;
        uint8_t *buf = (uint8_t *)IOMalloc(alloc);
        if (!buf) {
            IOLog("rtw88: OOM for %s (%zu bytes)\n", base, alloc);
            return NULL;
        }

        if (rtw88_decompress(buf, &out_len,
                              b->data, (uint32_t)b->compressed_size) != 0) {
            IOLog("rtw88: decompress failed for %s\n", base);
            IOFree(buf, alloc);
            return NULL;
        }

        struct firmware *fw = (struct firmware *)IOMallocZero(sizeof(*fw));
        if (!fw) {
            IOFree(buf, alloc);
            return NULL;
        }
        fw->data        = buf;
        fw->size        = out_len;   /* actual bytes from total_out */
        fw->_alloc_size = alloc;     /* original IOMalloc size for IOFree */
        IOLog("rtw88: firmware %s loaded from blob (%u bytes)\n",
              base, out_len);
        return fw;
    }

    IOLog("rtw88: firmware %s not in embedded blobs\n", base);
    return NULL;
}

static struct firmware *load_fw(const char *name)
{
    const char *base = name;
    for (const char *p = name; *p; p++)
        if (*p == '/') base = p + 1;

    return load_fw_from_blob(base);
}

/* ------------------------------------------------------------------ */
/*  Public API — signatures match linux/firmware.h                    */
/* ------------------------------------------------------------------ */

struct module;
struct device;

int request_firmware_nowait(struct module *module, int uevent,
                             const char *name, struct device *device,
                             unsigned int gfp, void *context,
                             void (*cont)(const struct firmware *fw, void *ctx))
{
    (void)module; (void)uevent; (void)device; (void)gfp;
    if (!cont) return -22;
    struct firmware *fw = load_fw(name);
    cont(fw, context);
    return 0;
}

void release_firmware(const struct firmware *fw)
{
    if (!fw) return;
    if (fw->data) IOFree((void *)fw->data, fw->_alloc_size);
    IOFree((void *)fw, sizeof(*fw));
}

int rtw88_load_firmware_sync(const char *name, const struct firmware **fw_out)
{
    *fw_out = load_fw(name);
    return *fw_out ? 0 : -2;
}

void rtw88_set_fw_dir(const char *dir) { (void)dir; }
void rtw88_find_fw_dir(void) {}
