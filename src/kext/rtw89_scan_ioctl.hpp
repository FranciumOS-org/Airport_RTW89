// SPDX-License-Identifier: GPL-2.0
#ifndef RTW89_SCAN_IOCTL_HPP
#define RTW89_SCAN_IOCTL_HPP
#include <stddef.h>
#include <IOKit/80211/apple80211_ioctl.h>

/* Sequoia's IO80211 uses _IOW/_IOWR with a 40-byte apple80211req.
 * The imported kernel headers encode 48 bytes in SIOCSA80211/SIOCGA80211.
 * Only bridge scan operations for the 40-byte commands; leave association
 * and every other ioctl on their existing family path. */
static inline bool rtw89ScanSet40(unsigned long cmd, unsigned int type) {
    return cmd == 0x802869c8UL && (type == 10 || type == 86);
}
static inline bool rtw89ScanGet40(unsigned long cmd, unsigned int type) {
    return cmd == 0xc02869c9UL && type == 11;
}

static inline uint32_t rtw89ScanWord(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Captured Sequoia layouts: single=5528 bytes, multiple=5352 bytes.
 * The latter has TEN 40-byte SSID entries, not the SDK's sixteen; its
 * num_channels/channels fields are at528/532 rather than768/772.
 * Normalize to one legacy request because the supported target is one SSID;
 * multi-name/multi-channel requests retain the existing wildcard/full scan. */
static inline bool rtw89DecodeHostScan(unsigned type, const void *data, size_t size,
                                      apple80211_scan_data &out) {
    if (!data) return false;
    const uint8_t *b = static_cast<const uint8_t *>(data);
    size_t countOffset, channelOffset, ssidOffset;
    unsigned names = 1;
    uint32_t length = 0;
    if (type == 10 && (size == 5528 || size == sizeof(apple80211_scan_data))) {
        countOffset = 68; channelOffset = 72; ssidOffset = 20;
        length = rtw89ScanWord(b + 16);
        if (length > APPLE80211_MAX_SSID_LEN) return false;
    } else if (type == 86 &&
               (size == 5352 || size == sizeof(apple80211_scan_multiple_data))) {
        const unsigned capacity = size == 5352 ? 10 : 16;
        countOffset = size == 5352 ? 528 : 768;
        channelOffset = countOffset + 4; ssidOffset = 20;
        names = rtw89ScanWord(b + 8);
        if (names > capacity) return false;
        for (unsigned i = 0; i < names; i++)
            if (rtw89ScanWord(b + 16 + i * 40) > APPLE80211_MAX_SSID_LEN) return false;
        if (names == 1) length = rtw89ScanWord(b + 16);
    } else return false;
    if (rtw89ScanWord(b) != APPLE80211_VERSION) return false;
    uint32_t channels = rtw89ScanWord(b + countOffset);
    if (channels > (size - channelOffset) / sizeof(apple80211_channel)) return false;
    out = {};
    out.version = APPLE80211_VERSION;
    out.bss_type = rtw89ScanWord(b + 4);
    out.ssid_len = length;
    for (uint32_t i = 0; i < length; i++) out.ssid[i] = b[ssidOffset + i];
    if (channels == 1) {
        out.num_channels = 1;
        out.channels[0].version = rtw89ScanWord(b + channelOffset);
        out.channels[0].channel = rtw89ScanWord(b + channelOffset + 4);
        out.channels[0].flags = rtw89ScanWord(b + channelOffset + 8);
    }
    return true;
}
#endif
