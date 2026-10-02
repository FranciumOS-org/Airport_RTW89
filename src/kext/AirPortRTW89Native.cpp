// SPDX-License-Identifier: GPL-2.0
/*
 * Native Wi-Fi: the driver's side of the seam to AirPortRTW89Front
 * (src/front/rtw89_front_api.h). When the front is in the registry, the kext
 * does not publish an Ethernet interface of its own; it connects to the front
 * and answers what IO80211 asks: scan, join, state, and the data frames of the
 * Wi-Fi interface. The requests and their answers follow AirPort_RTW88's
 * controller (reference/airport_rtw88/kext/AirportRTW88.cpp), which follows
 * AirportItlwm; the structures are the old family's (Ventura), which is what
 * OpenCore puts back on later systems.
 */
#include "AirPortRTW89.hpp"

#include <IOKit/80211/apple80211_ioctl.h>
#include <IOKit/80211/apple80211_var.h>
#include <IOKit/IOLib.h>
#include <libkern/version.h>
#include <sys/errno.h>
#include <sys/kpi_mbuf.h>
#include <sys/systm.h>

#define LOG(fmt, ...) IOLog("AirPort_RTW89: " fmt "\n", ##__VA_ARGS__)

static_assert(sizeof(struct apple80211_scan_result) == 1164, "scan result layout is not Ventura's");

struct AirPort_RTW89::NativeState {
    struct rtw89_front_ops front = {};
    struct apple80211_scan_result scanResult = {};
    unsigned int scanCursor = 0;
    bool scanInProgress = false;
    bool scanBootstrapTried = false;
    uint32_t authLower = APPLE80211_AUTHTYPE_OPEN;
    uint32_t authUpper = APPLE80211_AUTHTYPE_NONE;
    uint8_t countryCode[APPLE80211_MAX_CC_LEN] = { 'Z', 'Z', 0 };
    uint32_t powerSave = APPLE80211_POWERSAVE_MODE_DISABLED;
    uint8_t rsnIe[64] = {};             /* set by IO80211 for the next join */
    uint8_t rsnIeLen = 0;
    uint8_t roamProfile[76] = {};
    bool roamProfileValid = false;
    volatile UInt32 assocRequests = 0;  /* ASSOCIATE requests that reached us */
    bool linkUp = false;
    bool wasJoining = false;
    uint32_t deauthReason = 0;
};

/* ------------------------------------------------------------------ */
/*  Small helpers                                                       */
/* ------------------------------------------------------------------ */

/*
 * Working memory from the heap. IO80211 calls back into the controller from
 * inside its own requests (a power change makes it ask for the BSSID before
 * the first call has returned), on a kernel stack of 16 KB: buffers on the
 * stack of the request handler overflowed it.
 */
struct Scratch {
    void *p;
    size_t n;
    explicit Scratch(size_t size) : p(IOMallocZero(size)), n(size) {}
    ~Scratch() { if (p) { bzero(p, n); IOFree(p, n); } }
    uint8_t *bytes() const { return static_cast<uint8_t *>(p); }
    Scratch(const Scratch &) = delete;
    Scratch &operator=(const Scratch &) = delete;
};

static unsigned int channelOfFreq(unsigned int freq)
{
    if (freq == 2484)
        return 14;
    if (freq >= 2412 && freq <= 2472)
        return (freq - 2407) / 5;
    if (freq >= 5000 && freq < 5925)
        return (freq - 5000) / 5;
    return 0;
}

/* What apple80211Request() returns is an IOReturn or a small errno; the BSD
 * ioctl path wants an errno. */
static int errnoOf(SInt32 r)
{
    if (r >= 0 && r < 256)
        return r;
    switch (r) {
    case kIOReturnNoMemory:     return ENOMEM;
    case kIOReturnBadArgument:  return EINVAL;
    case kIOReturnUnsupported:  return ENOTSUP;
    case kIOReturnBusy:         return EBUSY;
    case kIOReturnNotReady:     return ENXIO;
    case kIOReturnNotFound:     return ENOENT;
    case kIOReturnNoSpace:      return ENOSPC;
    default:                    return EIO;
    }
}

/* Upper authentication types whose sign-in is 802.1X (WPA/WPA2-Enterprise). */
static const uint32_t kEnterpriseAuth = APPLE80211_AUTHTYPE_WPA | APPLE80211_AUTHTYPE_WPA2 |
                                        APPLE80211_AUTHTYPE_8021X | APPLE80211_AUTHTYPE_SHA256_8021X;
static const uint32_t kPersonalAuth = APPLE80211_AUTHTYPE_WPA_PSK | APPLE80211_AUTHTYPE_WPA2_PSK |
                                      APPLE80211_AUTHTYPE_SHA256_PSK;

static bool isEnterprise(uint32_t upper)
{
    return (upper & kEnterpriseAuth) && !(upper & kPersonalAuth);
}

static bool isConnected(const struct rtw89_glue_link &link)
{
    return link.state == RTW89_GLUE_LINK_CONNECTED;
}

static bool isJoining(const struct rtw89_glue_link &link)
{
    return link.state == RTW89_GLUE_LINK_JOINING || link.state == RTW89_GLUE_LINK_ASSOCIATED;
}

static uint32_t channelFlags(unsigned int channel, unsigned int width)
{
    return APPLE80211_C_FLAG_ACTIVE |
           (width >= 80 ? APPLE80211_C_FLAG_80MHZ :
            width >= 40 ? APPLE80211_C_FLAG_40MHZ : APPLE80211_C_FLAG_20MHZ) |
           (channel <= 14 ? APPLE80211_C_FLAG_2GHZ : APPLE80211_C_FLAG_5GHZ);
}

/* A network of the scan list in the form IO80211 wants. @fullIes: all of its
 * elements (the current network), else only the RSN element, which is what
 * the scan list of the reference drivers carries. */
static void fillScanResult(const struct rtw89_glue_bss &bss, const uint8_t *ies, size_t iesLen,
                           uint16_t beaconInt, struct apple80211_scan_result *d, bool fullIes)
{
    bzero(d, sizeof(*d));
    d->version = APPLE80211_VERSION;
    d->asr_channel.version = APPLE80211_VERSION;
    d->asr_channel.channel = bss.channel;
    d->asr_channel.flags = channelFlags(bss.channel, 20);
    d->asr_noise = -95;
    d->asr_rssi = bss.signal;
    d->asr_snr = (int16_t)(bss.signal - d->asr_noise);
    d->asr_beacon_int = (int16_t)beaconInt;
    d->asr_cap = (int16_t)bss.capability;
    memcpy(d->asr_bssid, bss.bssid, APPLE80211_ADDR_LEN);
    d->asr_ssid_len = bss.ssid_len > APPLE80211_MAX_SSID_LEN ? APPLE80211_MAX_SSID_LEN : bss.ssid_len;
    memcpy(d->asr_ssid, bss.ssid, d->asr_ssid_len);

    if (fullIes) {
        size_t n = iesLen > sizeof(d->asr_ie_data) ? sizeof(d->asr_ie_data) : iesLen;

        memcpy(d->asr_ie_data, ies, n);
        d->asr_ie_len = (int16_t)n;
    }
    for (size_t pos = 0; pos + 2 <= iesLen;) {
        size_t n = (size_t)ies[pos + 1] + 2;

        if (n > iesLen - pos)
            break;
        if (!fullIes && ies[pos] == 48 && !d->asr_ie_len && n <= sizeof(d->asr_ie_data)) {
            memcpy(d->asr_ie_data, ies + pos, n);
            d->asr_ie_len = (int16_t)n;
        }
        if (ies[pos] == 1 || ies[pos] == 50) {
            for (size_t j = 2; j < n && d->asr_nrates < APPLE80211_MAX_RATES; j++)
                d->asr_rates[d->asr_nrates++] = ies[pos + j];
        }
        pos += n;
    }
}

/*
 * The join request as Sequoia (900 bytes) and Tahoe (908 bytes) send it. The
 * old family would cut it down to Ventura's structure, where mode and both
 * authentication fields are narrower. Only the shared first 108 bytes are
 * read (RTW88AssocWire in the reference).
 */
struct HostAssoc {
    uint32_t upper, ssidLen, keyLen, cipher;
    uint8_t ssid[32], bssid[6], key[32];
};

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool decodeHostAssoc(const uint8_t *p, struct HostAssoc *out)
{
    uint32_t upper = le32(p + 12), ssidLen = le32(p + 16), keyLen = le32(p + 64);

    if (le32(p) != 1 || le32(p + 4) != 2 || le32(p + 8) != 1 || le32(p + 60) != 1)
        return false;
    if ((upper != 0 && upper != 8 && !isEnterprise(upper)) || !ssidLen || ssidLen > 32 ||
        (upper == 8 ? keyLen != 32 : keyLen != 0))
        return false;
    for (uint32_t i = 0; i < ssidLen; i++)
        if (!p[20 + i])
            return false;
    if (p[52] & 1)
        return false;
    bzero(out, sizeof(*out));
    out->upper = upper;
    out->ssidLen = ssidLen;
    out->keyLen = keyLen;
    out->cipher = le32(p + 68);
    memcpy(out->ssid, p + 20, ssidLen);
    memcpy(out->bssid, p + 52, 6);
    memcpy(out->key, p + 76, keyLen);
    return true;
}

static bool assocLooksValid(const struct apple80211_assoc_data *d)
{
    const uint32_t personal = APPLE80211_AUTHTYPE_WPA_PSK | APPLE80211_AUTHTYPE_WPA2_PSK |
                              APPLE80211_AUTHTYPE_SHA256_PSK;
    bool secure;

    if (!d || d->version != APPLE80211_VERSION)
        return false;
    if (d->ad_mode != APPLE80211_AP_MODE_UNKNOWN && d->ad_mode != APPLE80211_AP_MODE_INFRA &&
        d->ad_mode != APPLE80211_AP_MODE_ANY)
        return false;
    if (!d->ad_ssid_len || d->ad_ssid_len > APPLE80211_MAX_SSID_LEN)
        return false;
    for (uint32_t i = 0; i < d->ad_ssid_len; i++)
        if (!d->ad_ssid[i])
            return false;
    secure = (d->ad_auth_upper & personal) != 0;
    if (d->ad_auth_lower != APPLE80211_AUTHTYPE_OPEN)
        return false;
    if (isEnterprise(d->ad_auth_upper))
        return true;
    if (d->ad_auth_upper != APPLE80211_AUTHTYPE_NONE && !secure)
        return false;
    return secure ? d->ad_key.key_len == 32 : d->ad_key.key_len == 0;
}

/* ------------------------------------------------------------------ */
/*  Coming and going                                                    */
/* ------------------------------------------------------------------ */

enum { kNativeEventLink = 1, kNativeEventScanDone = 2, kNativeEventScanAborted = 4 };

void AirPort_RTW89::nativeEventsCall(thread_call_param_t self, thread_call_param_t)
{
    AirPort_RTW89 *me = static_cast<AirPort_RTW89 *>(self);

    OSIncrementAtomic(&me->_nativeRunning);
    if (!me->_nativeClosing)
        me->nativeEvents();
    OSDecrementAtomic(&me->_nativeRunning);
}

/* Any context. */
void AirPort_RTW89::nativeNotify(UInt32 events)
{
    if (!_native || _nativeClosing || !_nativeCall)
        return;
    OSBitOrAtomic(events, &_nativeEvents);
    thread_call_enter(_nativeCall);
}

void AirPort_RTW89::nativeEvents()
{
    UInt32 events;

    do {
        events = _nativeEvents;
    } while (!OSCompareAndSwap(events, 0, &_nativeEvents));
    if (!_native || !_ns)
        return;

    if (events & (kNativeEventScanDone | kNativeEventScanAborted)) {
        UInt32 result = events & kNativeEventScanAborted ? 1 : 0;

        _ns->scanInProgress = false;
        _ns->scanCursor = 0;
        nativePost(APPLE80211_M_SCAN_DONE, &result, sizeof(result));
    }
    if (events & kNativeEventLink) {
        struct rtw89_glue_link link;

        rtw89_glue_link(&link);
        nativeLink(link);
    }
}

/* interface->postMessage(), once the front has handed over its functions */
void AirPort_RTW89::nativePost(unsigned int msg, void *data, size_t len)
{
    if (_ns && _ns->front.post_message)
        _ns->front.post_message(_ns->front.ctx, msg, data, len);
}

/* Called from start(), after the probe. True: the front is there and now
 * ours; the kext must not publish its own interface. */
bool AirPort_RTW89::connectFront()
{
    OSDictionary *matching = serviceMatching(RTW89_FRONT_CLASS);
    const OSSymbol *name = OSSymbol::withCString(RTW89_FRONT_CONNECT);
    struct rtw89_back_ops back = {};
    IOService *front = nullptr;
    IOReturn ret;

    if (matching) {
        /* already there if it is there at all: OpenCore injected it at boot */
        front = waitForMatchingService(matching, 100 * 1000 * 1000ULL);
        matching->release();
    }
    if (!front || !name) {
        OSSafeReleaseNULL(name);
        OSSafeReleaseNULL(front);
        return false;
    }

    _ns = new NativeState;
    _nativeCall = thread_call_allocate(nativeEventsCall, this);
    back.version = RTW89_FRONT_API_VERSION;
    back.ctx = this;
    back.get_mac = backGetMac;
    back.set_mac = backSetMac;
    back.request = backRequest;
    back.ioctl = backIoctl;
    back.ioctl_set = backIoctlSet;
    back.enable = backEnable;
    back.output = backOutput;
    back.power = backPower;

    _native = true;         /* the callbacks below may come during the call */
    ret = front->callPlatformFunction(name, true, &back, &_ns->front, nullptr, nullptr);
    name->release();
    if (ret != kIOReturnSuccess) {
        LOG("the front refused the connection: 0x%x", ret);
        _native = false;
        delete _ns;
        _ns = nullptr;
        front->release();
        return false;
    }
    _front = front;
    _dataReady = true;
    /* macOS decides when to join and when to join again */
    rtw89_glue_set_rejoin(false);
    LOG("connected to %s: native Wi-Fi", RTW89_FRONT_CLASS);
    return true;
}

void AirPort_RTW89::disconnectFront()
{
    const OSSymbol *name;

    if (!_front)
        return;
    if (_ns->front.set_apple_rsn)
        _ns->front.set_apple_rsn(_ns->front.ctx, false);
    _nativeClosing = true;
    if (_nativeCall) {
        /* nothing new is queued now; let one that is running finish */
        thread_call_cancel(_nativeCall);
        IOSleep(10);
        while (_nativeRunning)
            IOSleep(1);
        thread_call_free(_nativeCall);
        _nativeCall = nullptr;
    }
    if (_ns->linkUp) {
        _ns->linkUp = false;
        _ns->front.set_link(_ns->front.ctx, false, 0, 0);
    }
    nativePost(APPLE80211_M_POWER_CHANGED);
    name = OSSymbol::withCString(RTW89_FRONT_DISCONNECT);
    if (name) {
        /* returns when no call from the front is in progress any more */
        _front->callPlatformFunction(name, true, nullptr, nullptr, nullptr, nullptr);
        name->release();
    }
    _native = false;
    _front->release();
    _front = nullptr;
    delete _ns;
    _ns = nullptr;
}

/* ------------------------------------------------------------------ */
/*  From the driver: frames, link, scans                                */
/* ------------------------------------------------------------------ */

void AirPort_RTW89::nativeRxFrame(const uint8_t *frame, size_t len)
{
    mbuf_t m = _ns->front.alloc_packet(_ns->front.ctx, (unsigned int)len);

    if (!m)
        return;
    if (mbuf_copyback(m, 0, len, frame, MBUF_DONTWAIT) != 0) {
        mbuf_freem(m);
        return;
    }
    _ns->front.input_packet(_ns->front.ctx, m);
}

void AirPort_RTW89::nativeTxWake()
{
    _ns->front.tx_wake(_ns->front.ctx);
}

/* The link state changed (publishLink() has the new one). */
void AirPort_RTW89::nativeLink(const struct rtw89_glue_link &link)
{
    bool up = isConnected(link);
    bool joining = isJoining(link);

    if (up && !_ns->linkUp) {
        uint64_t mbit = link.mode >= 3 ? (link.width >= 80 ? 1201 : link.width >= 40 ? 574 : 287) :
                        link.mode == 2 ? (link.width >= 80 ? 866 : link.width >= 40 ? 400 : 173) :
                        link.mode == 1 ? (link.width >= 40 ? 300 : 144) : 54;

        _ns->linkUp = true;
        _ns->front.set_link(_ns->front.ctx, true, mbit * 1000000ULL, 0);
        /* the join macOS asked for is complete: associated and keys in place */
        nativePost(APPLE80211_M_ASSOC_DONE, nullptr, 0);
        LOG("native link up");
    } else if (!up && _ns->linkUp) {
        /* -2000 - reason: the AP sent us off */
        _ns->deauthReason = link.last_error <= -2000 ? (uint32_t)(-2000 - link.last_error) : 0;
        _ns->linkUp = false;
        _ns->front.set_link(_ns->front.ctx, false, 0, _ns->deauthReason);
        if (_ns->deauthReason)
            nativePost(APPLE80211_M_DEAUTH_RECEIVED, nullptr, 0);
        nativePost(APPLE80211_M_LINK_CHANGED, nullptr, 0);
        LOG("native link down");
    } else if (!up && !joining && _ns->wasJoining) {
        /* a join that did not get there: macOS is waiting to hear */
        nativePost(APPLE80211_M_ASSOC_DONE, nullptr, 0);
        LOG("native join failed (%d)", link.last_error);
    }
    _ns->wasJoining = joining;
}

/* A driver thread, with its locks held. */
void AirPort_RTW89::scanDone(void *ctx, bool aborted)
{
    static_cast<AirPort_RTW89 *>(ctx)->nativeNotify(aborted ? kNativeEventScanAborted :
                                                              kNativeEventScanDone);
}

/* ------------------------------------------------------------------ */
/*  From the front: rtw89_back_ops                                      */
/* ------------------------------------------------------------------ */

void AirPort_RTW89::backGetMac(void *ctx, uint8_t mac[6])
{
    struct rtw89_glue_info info = {};

    rtw89_glue_get_info(&info);
    memcpy(mac, info.mac, 6);
}

int AirPort_RTW89::backSetMac(void *ctx, const uint8_t mac[6])
{
    AirPort_RTW89 *me = static_cast<AirPort_RTW89 *>(ctx);
    int ret;

    IOLockLock(me->_commandLock);
    ret = rtw89_glue_set_mac(mac);
    IOLockUnlock(me->_commandLock);
    LOG("station address set to %02x:%02x:%02x:%02x:%02x:%02x: %d", mac[0], mac[1], mac[2],
        mac[3], mac[4], mac[5], ret);
    return ret;
}

int AirPort_RTW89::backEnable(void *ctx, bool on)
{
    AirPort_RTW89 *me = static_cast<AirPort_RTW89 *>(ctx);
    int ret = 0;

    IOLockLock(me->_commandLock);
    if (on)
        ret = rtw89_glue_up();
    else
        rtw89_glue_down();
    IOLockUnlock(me->_commandLock);
    LOG("interface %s: %d", on ? "enabled" : "disabled", ret);
    return ret;
}

void AirPort_RTW89::backPower(void *ctx, bool on)
{
    /* sleep and wake: not handled yet */
}

uint32_t AirPort_RTW89::backOutput(void *ctx, mbuf_t m)
{
    AirPort_RTW89 *me = static_cast<AirPort_RTW89 *>(ctx);
    size_t len = mbuf_pkthdr_len(m);
    uint8_t *frame = nullptr;
    void *handle;
    uint32_t result = kIOReturnOutputDropped;

    OSIncrementAtomic(&me->_txBusy);
    if (!me->_dataReady || !me->_ns->linkUp) {
        mbuf_freem(m);
    } else if (!rtw89_glue_tx_room()) {
        /* not consumed: the queue offers it again after tx_wake() */
        result = kIOReturnOutputStall;
    } else {
        if ((handle = rtw89_glue_tx_alloc(len, &frame))) {
            if (mbuf_copydata(m, 0, len, frame) == 0) {
                if (rtw89_glue_tx(handle) == 0)
                    result = kIOReturnOutputSuccess;
            } else {
                rtw89_glue_tx_cancel(handle);
            }
        }
        mbuf_freem(m);
    }
    OSDecrementAtomic(&me->_txBusy);
    return result;
}

int AirPort_RTW89::backRequest(void *ctx, unsigned int type, int number, void *interface, void *data)
{
    AirPort_RTW89 *me = static_cast<AirPort_RTW89 *>(ctx);

    if (type != SIOCGA80211 && type != SIOCSA80211)
        return kIOReturnBadArgument;
    if (!data && number != APPLE80211_IOC_DISASSOCIATE)
        return kIOReturnBadArgument;
    return me->nativeRequest(type == SIOCSA80211, number, data);
}

/*
 * The BSD ioctl in front of apple80211Request(). The old family unpacks most
 * requests itself; the exceptions are the ones whose layout differs on newer
 * systems or which it does not pass on (AirPort_RTW88's apple80211_ioctl()).
 */
int AirPort_RTW89::backIoctl(void *ctx, void *interface, void *vif, void *ifnet, unsigned long cmd,
                             void *data, bool *handled)
{
    AirPort_RTW89 *me = static_cast<AirPort_RTW89 *>(ctx);
    struct rtw89_front_ops *front = &me->_ns->front;
    struct apple80211req *req = static_cast<struct apple80211req *>(data);
    bool isGet = cmd == SIOCGA80211, isSet = cmd == SIOCSA80211;

    /* requests can arrive while the front is still attaching the interface,
     * before it has handed over its functions: those are the family's */
    if (!front->super_ioctl || !front->interface) {
        *handled = false;
        return 0;
    }
    *handled = true;
    if (version_major < 23 || !req || (!isGet && !isSet) || vif || interface != front->interface)
        return front->super_ioctl(front->ctx, interface, vif, ifnet, cmd, data);

    /* the wrapper is the family's to change: take what is needed first */
    const uint32_t reqType = (uint32_t)req->req_type;
    const uint32_t reqVal = (uint32_t)req->req_val;
    const uint32_t len = req->req_len;
    const user_addr_t reqData = (user_addr_t)req->req_data;

    if (isSet && reqType == APPLE80211_IOC_ASSOCIATE && (len == 900 || len == 908) && reqData) {
        uint8_t prefix[108];
        struct HostAssoc host;

        if (copyin(reqData, prefix, sizeof(prefix)) == 0 && decodeHostAssoc(prefix, &host)) {
            Scratch assocBuf(sizeof(struct apple80211_assoc_data));
            struct apple80211_assoc_data &assoc = *static_cast<struct apple80211_assoc_data *>(assocBuf.p);
            SInt32 r;

            assoc.version = APPLE80211_VERSION;
            assoc.ad_mode = APPLE80211_AP_MODE_INFRA;
            assoc.ad_auth_lower = APPLE80211_AUTHTYPE_OPEN;
            assoc.ad_auth_upper = host.upper;
            assoc.ad_ssid_len = host.ssidLen;
            memcpy(assoc.ad_ssid, host.ssid, host.ssidLen);
            memcpy(assoc.ad_bssid.octet, host.bssid, 6);
            assoc.ad_key.version = APPLE80211_VERSION;
            assoc.ad_key.key_len = host.keyLen;
            assoc.ad_key.key_cipher_type = host.cipher;
            memcpy(assoc.ad_key.key, host.key, host.keyLen);
            r = me->nativeRequest(true, APPLE80211_IOC_ASSOCIATE, &assoc);
            bzero(&assoc, sizeof(assoc));
            bzero(&host, sizeof(host));
            bzero(prefix, sizeof(prefix));
            return errnoOf(r);
        }
        bzero(prefix, sizeof(prefix));
    }

    const bool scanBridge = (isGet && reqType == APPLE80211_IOC_SCAN_RESULT) ||
                            (isSet && (reqType == APPLE80211_IOC_SCAN_REQ ||
                                       reqType == APPLE80211_IOC_SCAN_REQ_MULTIPLE));
    const bool scalarPowerSave = isSet && len == 0 && reqType == APPLE80211_IOC_POWERSAVE;
    const bool scalarDisassociate = isSet && len == 0 && reqType == APPLE80211_IOC_DISASSOCIATE;
    const bool channelsBridge = isGet && reqType == APPLE80211_IOC_CHANNELS_INFO;

    if (!scanBridge && !scalarPowerSave && !scalarDisassociate && !channelsBridge) {
        const UInt32 before = me->_ns->assocRequests;
        int sr = front->super_ioctl(front->ctx, interface, vif, ifnet, cmd, data);

        /* a join the family turned down before it got to us */
        if (isSet && reqType == APPLE80211_IOC_ASSOCIATE && (sr == ENOTSUP || sr == EOPNOTSUPP) &&
            me->_ns->assocRequests == before && len >= sizeof(struct apple80211_assoc_data) &&
            len <= 64U * 1024U && reqData) {
            Scratch assocBuf(sizeof(struct apple80211_assoc_data));
            struct apple80211_assoc_data &assoc = *static_cast<struct apple80211_assoc_data *>(assocBuf.p);

            if (copyin(reqData, &assoc, sizeof(assoc)) == 0 && assocLooksValid(&assoc))
                sr = errnoOf(me->nativeRequest(true, APPLE80211_IOC_ASSOCIATE, &assoc));
            bzero(&assoc, sizeof(assoc));
        }
        return sr;
    }

    if (len > 64U * 1024U)
        return EINVAL;
    if (len == 0) {
        if (scalarPowerSave) {
            me->_ns->powerSave = reqVal;
            return 0;
        }
        if (!scalarDisassociate)
            return front->super_ioctl(front->ctx, interface, vif, ifnet, cmd, data);
        return errnoOf(me->nativeRequest(true, reqType, nullptr));
    }
    if (!reqData)
        return EFAULT;

    if (channelsBridge) {
        if (len < sizeof(struct apple80211_channels_info))
            return EINVAL;
        void *out = IOMallocZero(len);
        if (!out)
            return ENOMEM;
        int e = errnoOf(me->nativeRequest(false, reqType, out));
        if (!e)
            e = copyout(out, reqData, len);
        IOFree(out, len);
        return e;
    }

    if (isGet && reqType == APPLE80211_IOC_SCAN_RESULT) {
        struct apple80211_scan_result *result = nullptr;
        int e = errnoOf(me->nativeRequest(false, reqType, &result));

        if (e)
            return e;
        if (!result)
            return ENOENT;
        void *out = IOMallocZero(len);
        if (!out)
            return ENOMEM;
        memcpy(out, result, len < sizeof(*result) ? len : sizeof(*result));
        e = copyout(out, reqData, len);
        IOFree(out, len);
        return e;
    }

    /* the scan requests */
    if (len < (reqType == APPLE80211_IOC_SCAN_REQ ? sizeof(struct apple80211_scan_data) :
                                                    sizeof(struct apple80211_scan_multiple_data)))
        return EINVAL;
    void *payload = IOMallocZero(len);
    if (!payload)
        return ENOMEM;
    int e = copyin(reqData, payload, len);
    if (!e)
        e = errnoOf(me->nativeRequest(true, reqType, payload));
    IOFree(payload, len);
    return e;
}

/* The other way requests come in on newer systems: already split into get
 * and set, with either the wrapper or the unpacked structure as @data. */
int AirPort_RTW89::backIoctlSet(void *ctx, void *interface, void *vif, void *skywalk, void *data,
                                bool *handled)
{
    AirPort_RTW89 *me = static_cast<AirPort_RTW89 *>(ctx);
    struct rtw89_front_ops *front = &me->_ns->front;
    const UInt32 before = me->_ns->assocRequests;
    int sr;

    if (!front->super_ioctl_set || !front->interface) {
        *handled = false;
        return 0;
    }
    *handled = true;
    if (data && interface == front->interface && !vif) {
        struct apple80211req *req = static_cast<struct apple80211req *>(data);

        if (req->req_type == APPLE80211_IOC_POWERSAVE && req->req_len == 0 &&
            req->req_val >= APPLE80211_POWERSAVE_MODE_DISABLED &&
            req->req_val <= APPLE80211_POWERSAVE_MODE_MAX_POWERSAVE) {
            me->_ns->powerSave = (uint32_t)req->req_val;
            return 0;
        }
    }

    sr = front->super_ioctl_set(front->ctx, interface, vif, skywalk, data);
    if (me->_ns->assocRequests != before || !data)
        return sr;

    if (assocLooksValid(static_cast<struct apple80211_assoc_data *>(data)))
        return me->nativeRequest(true, APPLE80211_IOC_ASSOCIATE, data);

    struct apple80211req *req = static_cast<struct apple80211req *>(data);
    if (req->req_type == APPLE80211_IOC_ASSOCIATE &&
        req->req_len >= sizeof(struct apple80211_assoc_data) && req->req_len <= 64U * 1024U &&
        req->req_data) {
        Scratch assocBuf(sizeof(struct apple80211_assoc_data));
            struct apple80211_assoc_data &assoc = *static_cast<struct apple80211_assoc_data *>(assocBuf.p);

        if (copyin((user_addr_t)req->req_data, &assoc, sizeof(assoc)) == 0 &&
            assocLooksValid(&assoc))
            sr = me->nativeRequest(true, APPLE80211_IOC_ASSOCIATE, &assoc);
        bzero(&assoc, sizeof(assoc));
    }
    return sr;
}

/* ------------------------------------------------------------------ */
/*  apple80211Request(): one item, get or set                           */
/* ------------------------------------------------------------------ */

int AirPort_RTW89::nativeAssociate(void *data)
{
    struct apple80211_assoc_data *d = static_cast<struct apple80211_assoc_data *>(data);
    static const uint8_t zero[6] = {};
    const uint32_t personal = APPLE80211_AUTHTYPE_WPA_PSK | APPLE80211_AUTHTYPE_WPA2_PSK |
                              APPLE80211_AUTHTYPE_SHA256_PSK;
    struct rtw89_glue_link link;
    const uint8_t *bssid, *pmk = nullptr;
    bool secure, enterprise;
    int ret;

    OSIncrementAtomic(&_ns->assocRequests);
    if (!d || d->version != APPLE80211_VERSION || !d->ad_ssid_len ||
        d->ad_ssid_len > APPLE80211_MAX_SSID_LEN)
        return kIOReturnBadArgument;

    secure = (d->ad_auth_upper & personal) != 0;
    enterprise = isEnterprise(d->ad_auth_upper);
    if (d->ad_auth_lower != APPLE80211_AUTHTYPE_OPEN ||
        (d->ad_auth_upper != APPLE80211_AUTHTYPE_NONE && !secure && !enterprise))
        return kIOReturnUnsupported;
    /* 802.1X sign-in and the key handshake after it are IO80211's; a shared
     * password's handshake is the driver's */
    if (_ns->front.set_apple_rsn)
        _ns->front.set_apple_rsn(_ns->front.ctx, enterprise);
    if (enterprise) {
        /* nothing to check: the keys come later */
    } else if (secure) {
        /* IO80211 hands over the pairwise master key; the 4-way handshake is
         * the driver's */
        if (d->ad_key.key_len != 32)
            return kIOReturnUnsupported;
        pmk = d->ad_key.key;
    } else if (d->ad_key.key_len) {
        return kIOReturnUnsupported;
    }
    bssid = memcmp(d->ad_bssid.octet, zero, 6) ? d->ad_bssid.octet : nullptr;

    IOLockLock(_commandLock);
    rtw89_glue_link(&link);
    if ((isConnected(link) || isJoining(link)) && strlen(link.ssid) == d->ad_ssid_len &&
        !memcmp(link.ssid, d->ad_ssid, d->ad_ssid_len) &&
        (!bssid || !memcmp(bssid, link.bssid, 6))) {
        /* the same request again while it is being carried out */
        IOLockUnlock(_commandLock);
        return kIOReturnSuccess;
    }
    if (link.state != RTW89_GLUE_LINK_DOWN)
        rtw89_glue_leave();
    _ns->authLower = d->ad_auth_lower;
    _ns->authUpper = d->ad_auth_upper;
    if (enterprise) {
        const uint8_t *ie = d->ad_rsn_ie[0] == 48 ? d->ad_rsn_ie :
                            _ns->rsnIeLen ? _ns->rsnIe : nullptr;
        size_t ieLen = ie ? (size_t)ie[1] + 2 : 0;

        ret = rtw89_glue_join_ext(d->ad_ssid, d->ad_ssid_len, bssid, ie, ieLen);
        if (ret == -2 && bssid)
            ret = rtw89_glue_join_ext(d->ad_ssid, d->ad_ssid_len, nullptr, ie, ieLen);
    } else {
        ret = rtw89_glue_join_pmk(d->ad_ssid, d->ad_ssid_len, bssid, pmk);
        if (ret == -2 && bssid) /* that access point is not in the list: any of the network's */
            ret = rtw89_glue_join_pmk(d->ad_ssid, d->ad_ssid_len, nullptr, pmk);
    }
    IOLockUnlock(_commandLock);
    LOG("join requested by macOS (%s, upper auth 0x%x, %u-byte name): %d",
        enterprise ? "802.1X" : secure ? "WPA2" : "open", d->ad_auth_upper, d->ad_ssid_len, ret);
    if (ret == -2)
        return ENOENT;
    return ret ? kIOReturnError : kIOReturnSuccess;
}

int AirPort_RTW89::nativeRequest(bool isSet, int number, void *data)
{
    struct rtw89_glue_link link;

    rtw89_glue_link(&link);

    switch (number) {
    case APPLE80211_IOC_SSID: {
        struct apple80211_ssid_data *d = static_cast<struct apple80211_ssid_data *>(data);

        if (isSet)
            return kIOReturnSuccess;    /* the join request carries the name */
        if (!isConnected(link))
            return ENXIO;
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->ssid_len = (uint32_t)strnlen(link.ssid, sizeof(d->ssid_bytes));
        memcpy(d->ssid_bytes, link.ssid, d->ssid_len);
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_AUTH_TYPE: {
        struct apple80211_authtype_data *d = static_cast<struct apple80211_authtype_data *>(data);

        if (isSet) {
            _ns->authLower = d->authtype_lower;
            _ns->authUpper = d->authtype_upper;
            /* said before the join is asked for: IO80211 decides here
             * whether its supplicant will run */
            if (_ns->front.set_apple_rsn)
                _ns->front.set_apple_rsn(_ns->front.ctx, isEnterprise(d->authtype_upper));
        } else {
            bzero(d, sizeof(*d));
            d->version = APPLE80211_VERSION;
            d->authtype_lower = _ns->authLower;
            d->authtype_upper = _ns->authUpper;
        }
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_ASSOCIATE:
        return isSet ? nativeAssociate(data) : kIOReturnUnsupported;

    case APPLE80211_IOC_DISASSOCIATE:
        if (!isSet)
            return kIOReturnUnsupported;
        /* sent as part of preparing a join as well: not while one is under way */
        if (isJoining(link))
            return kIOReturnSuccess;
        IOLockLock(_commandLock);
        rtw89_glue_leave();
        IOLockUnlock(_commandLock);
        return kIOReturnSuccess;

    case APPLE80211_IOC_CIPHER_KEY: {
        struct apple80211_key *key = static_cast<struct apple80211_key *>(data);
        uint64_t rsc = 0;
        bool pairwise;
        int ret;

        if (!isSet)
            return kIOReturnUnsupported;
        if (key->version != APPLE80211_VERSION || key->key_len > APPLE80211_KEY_BUFF_LEN)
            return kIOReturnBadArgument;
        if (key->key_cipher_type == APPLE80211_CIPHER_NONE)
            return kIOReturnSuccess;
        if (key->key_cipher_type != APPLE80211_CIPHER_AES_CCM)
            return kIOReturnUnsupported;
        /* as AirportItlwm reads them: flags 4 the pairwise key, 0 a group key */
        if (key->key_flags != 4 && key->key_flags != 0)
            return kIOReturnUnsupported;
        pairwise = key->key_flags == 4;
        for (unsigned int i = 0; i < key->key_rsc_len && i < 6; i++)
            rsc |= (uint64_t)key->key_rsc[i] << (8 * i);
        ret = rtw89_glue_set_key(pairwise, pairwise ? 0 : key->key_index, key->key, key->key_len, rsc);
        LOG("%s key from IO80211 (index %u, %u bytes): %d", pairwise ? "pairwise" : "group",
            key->key_index, key->key_len, ret);
        /* -67: the handshake of this connection is the driver's own, the
         * key is not needed */
        if (ret && ret != -67)
            return kIOReturnError;
        nativePost(APPLE80211_M_RSN_HANDSHAKE_DONE);
        return kIOReturnSuccess;
    }

    case APPLE80211_IOC_RSN_IE:
    case APPLE80211_IOC_AP_IE_LIST: {
        struct rtw89_glue_bss bss;
        Scratch iesBuf(1024);
        uint8_t *ies = iesBuf.bytes();
        size_t len = 0;

        if (number == APPLE80211_IOC_RSN_IE) {
            struct apple80211_rsn_ie_data *d = static_cast<struct apple80211_rsn_ie_data *>(data);
            size_t own;

            if (isSet) {
                /* the element IO80211's supplicant wants us to associate with */
                _ns->rsnIeLen = 0;
                if (d->len >= 2 && d->len <= sizeof(_ns->rsnIe) && d->ie[0] == 48) {
                    memcpy(_ns->rsnIe, d->ie, d->len);
                    _ns->rsnIeLen = (uint8_t)d->len;
                }
                return kIOReturnSuccess;
            }
            /* ours, as sent: a handshake has to repeat it */
            own = rtw89_glue_assoc_rsn_ie(d->ie, sizeof(d->ie));
            if (own) {
                d->version = APPLE80211_VERSION;
                d->len = (uint16_t)own;
                return kIOReturnSuccess;
            }
        }
        if (isSet)
            return kIOReturnUnsupported;
        if (link.state == RTW89_GLUE_LINK_DOWN ||
            !rtw89_glue_find_bss(link.bssid, &bss, ies, 1024, &len, nullptr))
            return kIOReturnNotFound;
        for (size_t pos = 0; pos + 2 <= len; pos += (size_t)ies[pos + 1] + 2) {
            size_t n = (size_t)ies[pos + 1] + 2;

            if (n > len - pos)
                break;
            if (ies[pos] != 48)
                continue;
            if (number == APPLE80211_IOC_RSN_IE) {
                struct apple80211_rsn_ie_data *d = static_cast<struct apple80211_rsn_ie_data *>(data);

                if (n > sizeof(d->ie))
                    return kIOReturnNoSpace;
                d->version = APPLE80211_VERSION;
                d->len = (uint16_t)n;
                memcpy(d->ie, ies + pos, n);
            } else {
                struct apple80211_ap_ie_data *d = static_cast<struct apple80211_ap_ie_data *>(data);

                if (!d->ie_data || n > d->len)
                    return kIOReturnNoSpace;
                d->version = APPLE80211_VERSION;
                d->len = (uint32_t)n;
                memcpy(d->ie_data, ies + pos, n);
            }
            return kIOReturnSuccess;
        }
        return kIOReturnNotFound;
    }

    case APPLE80211_IOC_SCAN_REQ:
    case APPLE80211_IOC_SCAN_REQ_MULTIPLE: {
        int ret;

        if (!isSet)
            return kIOReturnUnsupported;
        /* a second request while one runs is answered by the same scan */
        if (_ns->scanInProgress)
            return kIOReturnSuccess;
        _ns->scanCursor = 0;
        if (link.state != RTW89_GLUE_LINK_DOWN) {
            /* connected: answered from what has been heard, without taking
             * the radio off the access point's channel */
            UInt32 result = 0;

            nativePost(APPLE80211_M_SCAN_DONE, &result, sizeof(result));
            return kIOReturnSuccess;
        }
        _ns->scanInProgress = true;
        IOLockLock(_commandLock);
        ret = rtw89_glue_scan();
        IOLockUnlock(_commandLock);
        if (ret) {
            _ns->scanInProgress = false;
            return ret == -16 ? kIOReturnBusy : kIOReturnError;
        }
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_SCANCACHE_CLEAR:
        if (!isSet)
            return kIOReturnUnsupported;
        _ns->scanCursor = 0;
        _ns->scanBootstrapTried = false;
        return kIOReturnSuccess;

    case APPLE80211_IOC_SCAN_RESULT: {
        struct apple80211_scan_result **out = static_cast<struct apple80211_scan_result **>(data);
        struct rtw89_glue_bss bss;
        Scratch iesBuf(1024);
        uint8_t *ies = iesBuf.bytes();
        uint16_t beaconInt = 100;
        size_t len = 0;

        if (isSet)
            return kIOReturnUnsupported;
        *out = nullptr;
        /* hidden networks have no name to show */
        while (rtw89_glue_scan_entry(_ns->scanCursor, &bss, ies, 1024, &len, &beaconInt)) {
            _ns->scanCursor++;
            if (!bss.ssid_len)
                continue;
            fillScanResult(bss, ies, len, beaconInt, &_ns->scanResult, false);
            *out = &_ns->scanResult;
            return kIOReturnSuccess;
        }
        if (_ns->scanInProgress && _ns->scanCursor == 0)
            return kIOReturnBusy;
        /* newer systems may only ever read the list: one scan of our own
         * when it is empty */
        if (_ns->scanCursor == 0 && !_ns->scanBootstrapTried &&
            link.state == RTW89_GLUE_LINK_DOWN && rtw89_glue_is_up()) {
            int ret;

            _ns->scanBootstrapTried = true;
            _ns->scanInProgress = true;
            IOLockLock(_commandLock);
            ret = rtw89_glue_scan();
            IOLockUnlock(_commandLock);
            if (ret) {
                _ns->scanInProgress = false;
                return kIOReturnError;
            }
            return kIOReturnBusy;
        }
        _ns->scanCursor = 0;
        return 5;       /* the end of the list */
    }
    case APPLE80211_IOC_CURRENT_NETWORK: {
        struct rtw89_glue_bss bss = {};
        Scratch iesBuf(1024);
        uint8_t *ies = iesBuf.bytes();
        uint16_t beaconInt = 100;
        size_t len = 0;

        if (isSet)
            return kIOReturnUnsupported;
        if (!isConnected(link))
            return kIOReturnNotReady;
        if (!rtw89_glue_find_bss(link.bssid, &bss, ies, 1024, &len, &beaconInt)) {
            /* not in the list any more: from what the connection itself knows */
            memcpy(bss.bssid, link.bssid, 6);
            bss.ssid_len = (uint8_t)strnlen(link.ssid, 32);
            memcpy(bss.ssid, link.ssid, bss.ssid_len);
            bss.channel = (uint8_t)channelOfFreq(link.freq);
            bss.signal = link.signal;
        }
        fillScanResult(bss, ies, len, beaconInt, static_cast<struct apple80211_scan_result *>(data),
                       true);
        return kIOReturnSuccess;
    }

    case APPLE80211_IOC_STATE: {
        struct apple80211_state_data *d = static_cast<struct apple80211_state_data *>(data);

        if (isSet)
            return kIOReturnUnsupported;
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->state = isConnected(link) ? APPLE80211_S_RUN :
                   link.state == RTW89_GLUE_LINK_ASSOCIATED ? APPLE80211_S_ASSOC :
                   link.state == RTW89_GLUE_LINK_JOINING ? APPLE80211_S_AUTH :
                   _ns->scanInProgress ? APPLE80211_S_SCAN : APPLE80211_S_INIT;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_CHANNEL: {
        struct apple80211_channel_data *d = static_cast<struct apple80211_channel_data *>(data);

        if (isSet)
            return kIOReturnUnsupported;
        if (!isConnected(link))
            return ENXIO;
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->channel.version = APPLE80211_VERSION;
        d->channel.channel = channelOfFreq(link.freq);
        d->channel.flags = channelFlags(d->channel.channel, link.width);
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_BSSID: {
        struct apple80211_bssid_data *d = static_cast<struct apple80211_bssid_data *>(data);

        if (isSet)
            return kIOReturnSuccess;
        if (!isConnected(link))
            return ENXIO;
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        memcpy(d->bssid.octet, link.bssid, 6);
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_RSSI: {
        struct apple80211_rssi_data *d = static_cast<struct apple80211_rssi_data *>(data);

        if (isSet)
            return kIOReturnUnsupported;
        if (!isConnected(link))
            return ENXIO;
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->num_radios = 1;
        d->rssi_unit = APPLE80211_UNIT_DBM;
        d->rssi[0] = d->aggregate_rssi = link.signal;
        d->rssi_ext[0] = d->aggregate_rssi_ext = link.signal;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_NOISE: {
        struct apple80211_noise_data *d = static_cast<struct apple80211_noise_data *>(data);

        if (isSet)
            return kIOReturnUnsupported;
        if (!isConnected(link))
            return ENXIO;
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->num_radios = 1;
        d->noise_unit = APPLE80211_UNIT_DBM;
        d->noise[0] = d->aggregate_noise = -95;
        d->noise_ext[0] = d->aggregate_noise_ext = -95;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_RATE: {
        struct apple80211_rate_data *d = static_cast<struct apple80211_rate_data *>(data);

        if (isSet)
            return kIOReturnUnsupported;
        if (!isConnected(link))
            return ENXIO;
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->num_radios = 1;
        d->rate[0] = link.tx_rate.kbps / 1000;      /* Mb/s */
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_RATE_SET: {
        struct apple80211_rate_set_data *d = static_cast<struct apple80211_rate_set_data *>(data);
        struct rtw89_glue_bss bss;
        Scratch iesBuf(1024);
        uint8_t *ies = iesBuf.bytes();
        size_t len = 0;

        if (isSet)
            return kIOReturnUnsupported;
        if (!isConnected(link) ||
            !rtw89_glue_find_bss(link.bssid, &bss, ies, 1024, &len, nullptr))
            return ENXIO;
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        for (size_t pos = 0; pos + 2 <= len; pos += (size_t)ies[pos + 1] + 2) {
            size_t n = (size_t)ies[pos + 1] + 2;

            if (n > len - pos)
                break;
            if (ies[pos] != 1 && ies[pos] != 50)
                continue;
            for (size_t j = 2; j < n && d->num_rates < APPLE80211_MAX_RATES; j++) {
                struct apple80211_rate *r = &d->rates[d->num_rates++];

                r->version = APPLE80211_VERSION;
                r->rate = ies[pos + j] & 0x7f;
                r->flags = ies[pos + j] & 0x80 ? APPLE80211_RATE_FLAG_BASIC : APPLE80211_RATE_FLAG_NONE;
            }
        }
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_MCS_INDEX_SET: {
        struct apple80211_mcs_index_set_data *d =
            static_cast<struct apple80211_mcs_index_set_data *>(data);

        if (isSet)
            return kIOReturnUnsupported;
        if (!isConnected(link))
            return ENXIO;
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        for (unsigned int mcs = 0; mcs < 8U * (link.nss ? link.nss : 1); mcs++)
            d->mcs_set_map[mcs / 8] |= (uint8_t)(1U << (mcs % 8));
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_MCS: {
        struct apple80211_mcs_data *d = static_cast<struct apple80211_mcs_data *>(data);

        if (isSet)
            return kIOReturnUnsupported;
        if (!isConnected(link))
            return ENXIO;
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->index = link.tx_rate.mcs;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_TX_NSS:
    case APPLE80211_IOC_NSS: {
        struct apple80211_nss_data *d = static_cast<struct apple80211_nss_data *>(data);

        if (isSet)
            return kIOReturnUnsupported;
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->nss = link.nss ? link.nss : 2;
        return kIOReturnSuccess;
    }

    case APPLE80211_IOC_OP_MODE: {
        struct apple80211_opmode_data *d = static_cast<struct apple80211_opmode_data *>(data);

        if (isSet)
            return kIOReturnUnsupported;
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->op_mode = APPLE80211_M_STA;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_PHY_MODE: {
        struct apple80211_phymode_data *d = static_cast<struct apple80211_phymode_data *>(data);

        if (isSet)
            return kIOReturnUnsupported;
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->phy_mode = APPLE80211_MODE_11A | APPLE80211_MODE_11B | APPLE80211_MODE_11G |
                      APPLE80211_MODE_11N | APPLE80211_MODE_11AC;
        /* Ventura's list ends at 802.11ac: an 802.11ax link is reported as that */
        d->active_phy_mode = link.state == RTW89_GLUE_LINK_DOWN ? APPLE80211_MODE_AUTO :
                             link.mode >= 2 ? APPLE80211_MODE_11AC :
                             link.mode == 1 ? APPLE80211_MODE_11N :
                             link.freq > 3000 ? APPLE80211_MODE_11A : APPLE80211_MODE_11G;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_CARD_CAPABILITIES: {
        struct apple80211_capability_data *d =
            static_cast<struct apple80211_capability_data *>(data);
        static const unsigned int caps[] = {
            APPLE80211_CAP_TKIP, APPLE80211_CAP_AES_CCM, APPLE80211_CAP_WPA2,
            APPLE80211_CAP_TKIPMIC, APPLE80211_CAP_SHSLOT, APPLE80211_CAP_SHPREAMBLE,
        };

        if (isSet)
            return kIOReturnUnsupported;
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        for (unsigned int cap : caps)
            d->capabilities[cap / 8] |= (uint8_t)(1U << (cap % 8));
        /* the rest as AirportItlwm publishes it for this family */
        d->capabilities[2] = 0xFF;
        d->capabilities[3] = 0x2B;
        d->capabilities[4] = 0xAD;
        d->capabilities[5] = 0x8C;
        d->capabilities[6] = 0x8C;
        d->capabilities[7] = 0x84;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_POWER: {
        struct apple80211_power_data *d = static_cast<struct apple80211_power_data *>(data);

        if (isSet) {
            int ret = 0;

            if (d->version != APPLE80211_VERSION || !d->num_radios ||
                d->num_radios > APPLE80211_MAX_RADIO)
                return kIOReturnBadArgument;
            if (d->power_state[0] != APPLE80211_POWER_ON && d->power_state[0] != APPLE80211_POWER_OFF)
                return kIOReturnUnsupported;
            IOLockLock(_commandLock);
            if (d->power_state[0] == APPLE80211_POWER_ON)
                ret = rtw89_glue_up();
            else
                rtw89_glue_down();
            IOLockUnlock(_commandLock);
            LOG("Wi-Fi turned %s by macOS: %d", d->power_state[0] == APPLE80211_POWER_ON ? "on" : "off",
                ret);
            if (ret)
                return kIOReturnError;
            nativePost(APPLE80211_M_POWER_CHANGED, nullptr, 0);
            return kIOReturnSuccess;
        }
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->num_radios = 1;
        d->power_state[0] = rtw89_glue_is_up() ? APPLE80211_POWER_ON : APPLE80211_POWER_OFF;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_POWERSAVE: {
        struct apple80211_powersave_data *d = static_cast<struct apple80211_powersave_data *>(data);

        if (isSet) {
            _ns->powerSave = d->powersave_level;
            return kIOReturnSuccess;
        }
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->powersave_level = _ns->powerSave;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_COUNTRY_CODE: {
        struct apple80211_country_code_data *d =
            static_cast<struct apple80211_country_code_data *>(data);

        if (isSet) {
            if (d->cc[0] != 'x' && d->cc[0] != 'X') {
                memcpy(_ns->countryCode, d->cc, sizeof(_ns->countryCode));
                _ns->countryCode[APPLE80211_MAX_CC_LEN - 1] = 0;
                nativePost(APPLE80211_M_COUNTRY_CODE_CHANGED, nullptr, 0);
            }
            return kIOReturnSuccess;
        }
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        memcpy(d->cc, _ns->countryCode, sizeof(d->cc));
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_LOCALE: {
        struct apple80211_locale_data *d = static_cast<struct apple80211_locale_data *>(data);

        if (isSet)
            return kIOReturnUnsupported;
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->locale = APPLE80211_LOCALE_ROW;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_SUPPORTED_CHANNELS:
    case APPLE80211_IOC_HW_SUPPORTED_CHANNELS: {
        struct apple80211_sup_channel_data *d =
            static_cast<struct apple80211_sup_channel_data *>(data);
        Scratch channelBuf(sizeof(struct rtw89_glue_channel) * APPLE80211_MAX_CHANNELS);
        struct rtw89_glue_channel *channels = static_cast<struct rtw89_glue_channel *>(channelBuf.p);
        unsigned int n;

        if (isSet)
            return kIOReturnUnsupported;
        n = rtw89_glue_channels(channels, APPLE80211_MAX_CHANNELS);
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->num_channels = n;
        for (unsigned int i = 0; i < n; i++) {
            d->supported_channels[i].version = APPLE80211_VERSION;
            d->supported_channels[i].channel = channels[i].channel;
            d->supported_channels[i].flags = channelFlags(channels[i].channel, 20);
        }
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_CHANNELS_INFO: {
        struct apple80211_channels_info *d = static_cast<struct apple80211_channels_info *>(data);
        Scratch channelBuf(sizeof(struct rtw89_glue_channel) * APPLE80211_MAX_CHANNELS);
        struct rtw89_glue_channel *channels = static_cast<struct rtw89_glue_channel *>(channelBuf.p);
        unsigned int n;

        if (isSet)
            return kIOReturnUnsupported;
        n = rtw89_glue_channels(channels, APPLE80211_MAX_CHANNELS);
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        for (unsigned int i = 0; i < n; i++) {
            d->chan_num[i] = channels[i].channel;
            d->passive[i] = channels[i].passive;
            d->radar_dfs[i] = channels[i].radar;
            d->support_40Mhz[i] = channels[i].channel != 14;
            d->support_80Mhz[i] = channels[i].channel > 14;
            d->chan_spec[i] = channels[i].channel;
        }
        d->num_chan_specs = (uint16_t)n;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_DRIVER_VERSION:
    case APPLE80211_IOC_HARDWARE_VERSION: {
        struct apple80211_version_data *d = static_cast<struct apple80211_version_data *>(data);

        if (isSet)
            return kIOReturnUnsupported;
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->string_len = (uint16_t)strlcpy(d->string, number == APPLE80211_IOC_DRIVER_VERSION ?
                                          "RTL8852BE (AirPort_RTW89 0.1.0)" : "RTL8852BE",
                                          sizeof(d->string));
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_RADIO_INFO: {
        struct apple80211_radio_info_data *d =
            static_cast<struct apple80211_radio_info_data *>(data);

        if (isSet)
            return kIOReturnUnsupported;
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->count = 1;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_TX_ANTENNA:
    case APPLE80211_IOC_ANTENNA_DIVERSITY: {
        struct apple80211_antenna_data *d = static_cast<struct apple80211_antenna_data *>(data);

        if (isSet)
            return kIOReturnUnsupported;
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->num_radios = 1;
        d->antenna_index[0] = 1;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_INT_MIT: {
        struct apple80211_intmit_data *d = static_cast<struct apple80211_intmit_data *>(data);

        if (isSet)
            return kIOReturnUnsupported;
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->int_mit = APPLE80211_INT_MIT_AUTO;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_PROTMODE: {
        struct apple80211_protmode_data *d = static_cast<struct apple80211_protmode_data *>(data);

        if (isSet)
            return kIOReturnUnsupported;
        if (!isConnected(link))
            return ENXIO;
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->protmode = APPLE80211_PROTMODE_OFF;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_TXPOWER: {
        struct apple80211_txpower_data *d = static_cast<struct apple80211_txpower_data *>(data);

        if (isSet)
            return kIOReturnUnsupported;
        if (!isConnected(link))
            return ENXIO;
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->txpower_unit = APPLE80211_UNIT_PERCENT;
        d->txpower = 100;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_ROAM_THRESH: {
        struct apple80211_roam_threshold_data *d =
            static_cast<struct apple80211_roam_threshold_data *>(data);

        if (isSet)
            return kIOReturnUnsupported;
        bzero(d, sizeof(*d));
        d->threshold = 100;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_ROAM_PROFILE:
        if (isSet) {
            memcpy(_ns->roamProfile, data, sizeof(_ns->roamProfile));
            _ns->roamProfileValid = true;
            return kIOReturnSuccess;
        }
        if (!_ns->roamProfileValid)
            return kIOReturnError;
        memcpy(data, _ns->roamProfile, sizeof(_ns->roamProfile));
        return kIOReturnSuccess;

    case APPLE80211_IOC_LINK_CHANGED_EVENT_DATA: {
        struct apple80211_link_changed_event_data *d =
            static_cast<struct apple80211_link_changed_event_data *>(data);

        if (isSet)
            return kIOReturnUnsupported;
        bzero(d, sizeof(*d));
        d->isLinkDown = !isConnected(link);
        if (d->isLinkDown) {
            d->reason = APPLE80211_LINK_DOWN_REASON_DEAUTH;
        } else {
            d->rssi = (uint32_t)link.signal;
            d->nf = (uint16_t)(int16_t)-95;
            d->snr = (uint16_t)(link.signal > -95 ? link.signal + 95 : 0);
        }
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_DEAUTH: {
        struct apple80211_deauth_data *d = static_cast<struct apple80211_deauth_data *>(data);

        if (isSet)
            return kIOReturnUnsupported;
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->deauth_reason = _ns->deauthReason;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_ASSOCIATION_STATUS: {
        struct apple80211_assoc_status_data *d =
            static_cast<struct apple80211_assoc_status_data *>(data);

        if (isSet)
            return kIOReturnUnsupported;
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->status = isConnected(link) ? APPLE80211_STATUS_SUCCESS : APPLE80211_STATUS_UNAVAILABLE;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_ASSOCIATE_RESULT: {
        struct apple80211_assoc_result_data *d =
            static_cast<struct apple80211_assoc_result_data *>(data);

        if (isSet)
            return kIOReturnUnsupported;
        if (!isConnected(link))
            return kIOReturnNotReady;
        d->version = APPLE80211_VERSION;
        d->result = APPLE80211_RESULT_SUCCESS;
        return kIOReturnSuccess;
    }

    case APPLE80211_IOC_ROAM:
    case APPLE80211_IOC_WOW_PARAMETERS:
    case APPLE80211_IOC_IE:
        return kIOReturnError;

    default:
        return kIOReturnUnsupported;
    }
}
