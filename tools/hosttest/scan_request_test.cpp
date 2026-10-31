// SPDX-License-Identifier: GPL-2.0
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../../src/kext/rtw89_scan_target.hpp"
#include "../../src/kext/rtw89_scan_ioctl.hpp"

int main(int argc, char **argv)
{
    // Actual Sequoia ioctl command words captured at the userspace boundary.
    // The old header constants encode 48 bytes and silently bypassed the
    // driver's BSD scan bridge, even though the request structure is 40.
    static_assert(sizeof(apple80211req) == 40, "scan envelope size");
    assert(SIOCSA80211 == 0x803069c8UL);
    assert(SIOCGA80211 == 0xc03069c9UL);
    assert(rtw89ScanSet40(0x802869c8UL, 10));
    assert(rtw89ScanSet40(0x802869c8UL, 86));
    assert(rtw89ScanGet40(0xc02869c9UL, 11));
    assert(!rtw89ScanSet40(0x802869c8UL, APPLE80211_IOC_ASSOCIATE));
    assert(!rtw89ScanGet40(0xc02869c9UL, APPLE80211_IOC_CIPHER_KEY));
    assert(!rtw89ScanSet40(0xc02869c9UL, 10));
    assert(!rtw89ScanGet40(0x802869c8UL, 11));
    // Wire fixtures use captured offsets, not sizeof/offsetof from the SDK.
    uint8_t wire[5352] = {};
    auto word = [](uint8_t *b, size_t offset, uint32_t value) {
        for (unsigned i = 0; i < 4; i++) b[offset+i] = (uint8_t)(value >> (8*i));
    };
    word(wire, 0, 1); word(wire, 4, 3); word(wire, 8, 1);
    word(wire, 12, 1); word(wire, 16, 8);
    memcpy(wire+20, "hot\0spot", 8);
    word(wire, 528, 1); word(wire, 532, 1);
    word(wire, 536, 149); word(wire, 540, APPLE80211_C_FLAG_5GHZ);
    apple80211_scan_data normalized = {};
    assert(rtw89DecodeHostScan(86, wire, sizeof(wire), normalized));
    assert(normalized.ssid_len == 8 && !memcmp(normalized.ssid, "hot\0spot", 8));
    assert(normalized.num_channels == 1 && normalized.channels[0].channel == 149);
    word(wire, 8, 11);
    assert(!rtw89DecodeHostScan(86, wire, sizeof(wire), normalized));
    word(wire, 8, 1); word(wire, 528, 1000);
    assert(!rtw89DecodeHostScan(86, wire, sizeof(wire), normalized));
    assert(!rtw89DecodeHostScan(86, wire, sizeof(wire)-1, normalized));
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
    // Reproduce ioctl retry semantics: a Busy result causes another request,
    // not merely another result read. Neither running nor completed retries
    // may start a fresh scan before the completed list has been consumed.
    Rtw89ScanCompletion completion;
    assert(!completion.reuse(name, sizeof(name), 5745));
    completion.begin(name, sizeof(name), 5745);
    assert(completion.busy());
    assert(completion.reuse(name, sizeof(name), 5745));
    assert(!completion.reuse(name, sizeof(name), 5180));
    completion.finish(false);
    assert(completion.ready && !completion.busy());
    assert(completion.reuse(name, sizeof(name), 5745));
    completion.consume();
    assert(!completion.reuse(name, sizeof(name), 5745));
    completion.begin(name, sizeof(name), 5745);
    completion.finish(true);
    assert(!completion.reuse(name, sizeof(name), 5745) && !completion.busy());
    if (argc == 2) {
        uint8_t captured[8192] = {};
        FILE *f = fopen(argv[1], "rb"); assert(f);
        size_t n = fread(captured, 1, sizeof(captured), f); fclose(f);
        assert(n == 5528 && rtw89DecodeHostScan(10, captured, n, normalized));
        assert(rtw89ScanTarget(10, &normalized, target, len, freq));
        assert(len == 14 && freq == 5745 && !memcmp(target, captured+20, len));
        puts("== captured Sequoia hotspot payload replay: target bytes and channel preserved");
    }
    puts("== native hotspot scan request: SSID bytes and band-specific channel preserved");
    puts("== directed scan ioctl retry: completed results consumed without rescan loop");
    puts("== Sequoia 40-byte scan ioctl: real command words reach scan bridge only");
    puts("== Sequoia multiple scan payload: 10 SSID slots and channel offsets validated");
}
