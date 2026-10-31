// SPDX-License-Identifier: GPL-2.0
#ifndef RTW89_SCAN_TARGET_HPP
#define RTW89_SCAN_TARGET_HPP
#include <IOKit/80211/apple80211_ioctl.h>

/* Pointers remain valid only while the caller owns the full native payload.
 * The glue copies the target before the request returns. Zero frequency
 * requests a full scan, preserving multi-channel and unspecified requests. */
static inline bool rtw89ScanTarget(int number, const void *data,
                                  const uint8_t *&target, uint32_t &targetLen,
                                  uint16_t &targetFreq)
{
    const struct apple80211_channel *targetChannel = nullptr;
    target = nullptr;
    targetLen = 0;
    targetFreq = 0;
    if (!data)
        return false;
    /* Select one explicit target in either native request form.
     * Multi-name background scans retain their existing wildcard behavior. */
    if (number == APPLE80211_IOC_SCAN_REQ) {
        const auto *d = static_cast<const struct apple80211_scan_data *>(data);
        if (d->ssid_len > APPLE80211_MAX_SSID_LEN ||
            d->num_channels > APPLE80211_MAX_CHANNELS)
            return false;
        target = d->ssid;
        targetLen = d->ssid_len;
        if (d->num_channels == 1)
            targetChannel = &d->channels[0];
    } else if (number == APPLE80211_IOC_SCAN_REQ_MULTIPLE) {
        const auto *d = static_cast<const struct apple80211_scan_multiple_data *>(data);
        if (d->ssid_count > 16 || d->num_channels > 128)
            return false;
        for (uint32_t i = 0; i < d->ssid_count; i++)
            if (d->ssids[i].ssid_len > APPLE80211_MAX_SSID_LEN)
                return false;
        if (d->ssid_count == 1) {
            target = d->ssids[0].ssid_bytes;
            targetLen = d->ssids[0].ssid_len;
        }
        if (d->num_channels == 1)
            targetChannel = &d->channels[0];
    }
    if (number != APPLE80211_IOC_SCAN_REQ && number != APPLE80211_IOC_SCAN_REQ_MULTIPLE)
        return false;
    if (targetLen && targetChannel) {
        unsigned int c = targetChannel->channel;
        if ((targetChannel->flags & 0x2000) && (c == 2 || (c >= 1 && c <= 233 && c % 4 == 1)))
            targetFreq = static_cast<uint16_t>(c == 2 ? 5935 : 5950 + 5 * c);
        else if ((targetChannel->flags & APPLE80211_C_FLAG_2GHZ) && c >= 1 && c <= 14)
            targetFreq = static_cast<uint16_t>(c == 14 ? 2484 : 2407 + 5 * c);
        else if ((targetChannel->flags & APPLE80211_C_FLAG_5GHZ) && c >= 32 && c <= 177)
            targetFreq = static_cast<uint16_t>(5000 + 5 * c);
        else
            return false;
    }
    return true;
}
#endif
