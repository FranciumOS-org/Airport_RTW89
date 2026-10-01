// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
#pragma once
#include <stdint.h>
#include <stddef.h>
// Copy response fields while the RX buffer is still owned by the caller.
static inline bool rtw88ResponsePeer(const uint8_t *f, size_t n,
                                    const uint8_t *mac, const uint8_t *bssid) {
    if (!f || !mac || !bssid || n < 30) return false;
    for (unsigned i = 0; i < 6; ++i)
        if (f[4+i] != mac[i] || f[10+i] != bssid[i] || f[16+i] != bssid[i]) return false;
    return true;
}
static inline bool rtw88AuthResponse(const uint8_t *f, size_t n,
    const uint8_t *mac, const uint8_t *bssid, uint16_t *status) {
    if (!status || !rtw88ResponsePeer(f,n,mac,bssid) || (f[0] & 0xfc) != 0xb0 ||
        f[24] != 0 || f[25] != 0 || f[26] != 2 || f[27] != 0) return false;
    *status = (uint16_t)(f[28] | ((uint16_t)f[29] << 8)); return true;
}
static inline bool rtw88AssocResponse(const uint8_t *f, size_t n,
    const uint8_t *mac, const uint8_t *bssid, uint16_t *status, uint16_t *aid) {
    if (!status || !aid || !rtw88ResponsePeer(f,n,mac,bssid) ||
        ((f[0] & 0xfc) != 0x10 && (f[0] & 0xfc) != 0x30)) return false;
    *status = (uint16_t)(f[26] | ((uint16_t)f[27] << 8));
    *aid = (uint16_t)((f[28] | ((uint16_t)f[29] << 8)) & 0x3fff);
    return *status != 0 || (*aid >= 1 && *aid <= 2007);
}

// Only the current AP may update STA signal. Promiscuous AWDL reception and
// scan traffic must not replace the infrastructure RSSI.
static inline bool rtw88StationSignal(const uint8_t *f, size_t n,
    const uint8_t *mac, const uint8_t *bssid, int signal, bool invalid) {
    if (!f || !mac || !bssid || n < 24 || invalid || signal >= 0 || signal < -127)
        return false;
    uint8_t any = 0;
    for (unsigned i=0;i<6;++i) any |= bssid[i];
    if (!any || (bssid[0]&1) || (f[0]&3)) return false;
    for (unsigned i=0;i<6;++i) if (f[10+i]!=bssid[i]) return false;
    if (!(f[4]&1)) {
        for (unsigned i=0;i<6;++i) if (f[4+i]!=mac[i]) return false;
    }
    const unsigned type = f[0]&0x0c, ds = f[1]&3;
    if (type == 8) return ds == 2; // AP -> STA data; TA is BSSID.
    if (type != 0 || ds != 0) return false;
    for (unsigned i=0;i<6;++i) if (f[16+i]!=bssid[i]) return false;
    return true;
}
