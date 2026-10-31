// SPDX-License-Identifier: GPL-2.0
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../../src/kext/rtw89_scan_target.hpp"

int main()
{
    const uint8_t name[] = {'h', 'o', 't', 0, 's', 'p', 'o', 't'};
    const uint8_t *target = nullptr;
    uint32_t len = 0;
    uint16_t freq = 0;
    apple80211_scan_data single = {};
    single.ssid_len = sizeof(name);
    memcpy(single.ssid, name, sizeof(name));
    single.num_channels = 1;
    single.channels[0].channel = 149;
    single.channels[0].flags = APPLE80211_C_FLAG_5GHZ;
    assert(rtw89ScanTarget(APPLE80211_IOC_SCAN_REQ, &single, target, len, freq));
    assert(len == sizeof(name) && !memcmp(target, name, len) && freq == 5745);

    apple80211_scan_multiple_data multiple = {};
    multiple.ssid_count = 1;
    multiple.ssids[0].ssid_len = sizeof(name);
    memcpy(multiple.ssids[0].ssid_bytes, name, sizeof(name));
    multiple.num_channels = 1;
    multiple.channels[0].channel = 6;
    multiple.channels[0].flags = APPLE80211_C_FLAG_2GHZ;
    assert(rtw89ScanTarget(APPLE80211_IOC_SCAN_REQ_MULTIPLE, &multiple, target, len, freq));
    assert(len == sizeof(name) && !memcmp(target, name, len) && freq == 2437);
    multiple.channels[0].channel = 5;
    multiple.channels[0].flags = 0x2000; // native 6 GHz band flag
    assert(rtw89ScanTarget(APPLE80211_IOC_SCAN_REQ_MULTIPLE, &multiple, target, len, freq));
    assert(freq == 5975);

    single.num_channels = 0;
    assert(rtw89ScanTarget(APPLE80211_IOC_SCAN_REQ, &single, target, len, freq));
    assert(freq == 0 && len == sizeof(name));
    single.ssid_len = 33;
    assert(!rtw89ScanTarget(APPLE80211_IOC_SCAN_REQ, &single, target, len, freq));
    multiple.ssid_count = 17;
    assert(!rtw89ScanTarget(APPLE80211_IOC_SCAN_REQ_MULTIPLE, &multiple, target, len, freq));
    multiple.ssid_count = 1;
    multiple.num_channels = 129;
    assert(!rtw89ScanTarget(APPLE80211_IOC_SCAN_REQ_MULTIPLE, &multiple, target, len, freq));
    assert(!rtw89ScanTarget(APPLE80211_IOC_SCAN_REQ, nullptr, target, len, freq));

    single = {};
    assert(rtw89ScanTarget(APPLE80211_IOC_SCAN_REQ, &single, target, len, freq));
    assert(len == 0 && freq == 0); // ordinary wildcard scan
    puts("== native hotspot scan request: SSID bytes and band-specific channel preserved");
}
