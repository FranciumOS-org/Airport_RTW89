// SPDX-License-Identifier: GPL-2.0
#ifndef RTW89_SCAN_TARGET_HPP
#define RTW89_SCAN_TARGET_HPP
#include <IOKit/80211/apple80211_ioctl.h>

/* Some native ioctl clients retry the scan REQUEST after SCAN_RESULT returns
 * Busy. Keep a completed directed request until its result list is consumed,
 * otherwise each retry starts another scan and can never read its results. */
struct Rtw89ScanCompletion {
    uint8_t ssid[APPLE80211_MAX_SSID_LEN] = {};
    uint32_t length = 0;
    uint16_t frequency = 0;
    bool pending = false;
    bool ready = false;
    bool reuse(const uint8_t *name, uint32_t len, uint16_t freq) const {
        if (!pending) return false;
        if (!len) return true; // background readers share the same result list
        if (!name || len != length || freq != frequency) return false;
        for (uint32_t i = 0; i < len; i++) if (name[i] != ssid[i]) return false;
        return true;
    }
    void begin(const uint8_t *name, uint32_t len, uint16_t freq) {
        pending = ready = false;
        if (!name || !len || len > sizeof(ssid)) return;
        for (uint32_t i = 0; i < len; i++) ssid[i] = name[i];
        length = len; frequency = freq; pending = true;
    }
    void finish(bool aborted) { if (aborted) consume(); else ready = pending; }
    void consume() { pending = ready = false; }
    bool busy() const { return pending && !ready; }
};

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
