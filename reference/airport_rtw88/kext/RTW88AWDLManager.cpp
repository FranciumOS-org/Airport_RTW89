/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#include <IOKit/80211/IO80211Controller.h>
#include <IOKit/80211/IO80211VirtualInterface.h>
#include <IOKit/80211/apple80211_ioctl.h>

#include "RTW88AWDLManager.hpp"
#include "RTW88IEEE80211.hpp"
#include <kern/clock.h>
#include <mach/mach_time.h>

namespace {
template <typename R> struct AWDLResult {
    R value{};
    template <typename F> void call(F &f) { value = f(); }
    R get() { return value; }
};
template <> struct AWDLResult<void> {
    template <typename F> void call(F &f) { f(); }
    void get() {}
};
// IOWorkLoop's gate is recursive. runAction also handles callers outside the
// workloop without reaching into protected APIs or introducing a second lock.
template <typename R, typename F>
R awdlGated(IOWorkLoop *loop, OSObject *owner, F fn)
{
    if (!loop) return fn();
    struct Task { F function; AWDLResult<R> result; } task{fn, {}};
    loop->runAction([](OSObject *, void *arg, void *, void *, void *) -> IOReturn {
        auto *task = static_cast<Task *>(arg);
        task->result.call(task->function);
        return kIOReturnSuccess;
    }, owner, &task);
    return task.result.get();
}
}

RTW88AWDLManager::~RTW88AWDLManager()
{
    reset();
}

bool RTW88AWDLManager::init(RTW88IEEE80211 *backend, IOWorkLoop *workLoop,
                              IOService *owner, IOTimerEventSource::Action action)
{
    if (!backend || !workLoop || !owner || !action)
        return false;
    _backend = backend;
    _workLoop = workLoop;
    _owner = owner;
    _timer = IOTimerEventSource::timerEventSource(owner, action);
    _txBuffer = (uint8_t *)IOMalloc(RTW88AWDL::MaxFrame);
    if (!_timer || !_txBuffer) return false;
    if (_workLoop->addEventSource(_timer) != kIOReturnSuccess) return false;
    _timerAttached = true;
    _syncEnabled = true;
    _p2pEnabled = false;
    _electionMetric = 0;
    _electionId = 0;
    _masterChannel = 0;
    _secondaryMasterChannel = 0;
    _presenceMode = 0;
    _syncState = 0;
    _actionFrameTxMode = 0;
    _minRate = 0;
    _deviceCapabilities = 0;
    memcpy(_bssid, RTW88AWDL::BSSID, sizeof(_bssid));
    _peerRegistrations = 0;
    publishStats();
    return true;
}

void RTW88AWDLManager::scheduleDiscovery()
{
    // Wake the control-plane state machine.  It first waits for IO80211 and
    // only synthesizes the OpenAWDL-derived bootstrap after the grace period.
    if (_timer && _timerAttached) _timer->setTimeoutMS(25);
}

void RTW88AWDLManager::suspendForPowerTransition()
{
    return awdlGated<void>(_workLoop, _owner, [&]() -> void {
        if (_backend)
            _backend->restoreSTAChannelAfterAWDL();
        if (_timer)
            _timer->cancelTimeout();
        flushData();
        flushActions();
        _nextActionUS = _nextPSFUS = 0;
        _lastMIFEAW = 0;
        _lastMIFEAWValid = false;
        _nextSocialSweepUS = 0;
        publishStats();
    });
}

void RTW88AWDLManager::resumeAfterPowerTransition()
{
    return awdlGated<void>(_workLoop, _owner, [&]() -> void {
        if (!_syncEnabled)
            return;
        const uint64_t now = nowUS();
        if (!_airTemplate) _fallbackDeadlineUS = now + 1500000ULL;
        _nextActionUS = _nextPSFUS = 0;
        _lastMIFEAW = 0;
        _lastMIFEAWValid = false;
        _nextSocialSweepUS = 0;
        publishStats();
        arm(_airTemplate ? 1 : 25);
    });
}

void RTW88AWDLManager::reset()
{
    return awdlGated<void>(_workLoop, _owner, [&]() -> void {
    /* Never release the backend while AWDL still owns the single PHY.  The
     * STA must be back on its AP channel before the AWDL manager disappears. */
    if (_backend) _backend->restoreSTAChannelAfterAWDL();
    if (_timer) {
        _timer->cancelTimeout();
        if (_timerAttached && _workLoop) _workLoop->removeEventSource(_timer);
        _timer->release(); _timer = nullptr; _timerAttached = false;
    }
    flushData();
    flushActions();
    if (_airTemplate) { IOFree(_airTemplate, _airTemplateLength); _airTemplate = nullptr; }
    _airTemplateLength = 0;
    _nativeSchedule = false; _masterSeenUS = 0;
    _appleControlMask = 0; _lastAppleControlUS = 0; _vifEnabledUS = 0; _fallbackDeadlineUS = 0;
    _nextActionUS = _nextPSFUS = 0; _lastMIFEAW = 0; _lastMIFEAWValid = false;
    _nextSocialSweepUS = 0; _socialSweeps = 0; _discoverySocialIndex = 0; _discoveryChannel = 6;
    _observedMasterChannel = 0; _nextAWChannel = 0; _masterChannelRaw = 0;
    _lastActionRxUS = 0; _mifRx = 0; _versionRx = 0;
    _mifPeersObserved = 0; _versionPeersObserved = 0;
    if (_txBuffer) { IOFree(_txBuffer, RTW88AWDL::MaxFrame); _txBuffer = nullptr; }
    _owner = nullptr;
    bzero(_peers, sizeof(_peers));
    _awdlInterface = nullptr;
    _p2pInterface = nullptr;
    _backend = nullptr;
    _workLoop = nullptr;
    _syncEnabled = true;
    _p2pEnabled = false;
    _electionMetric = 0;
    _electionId = 0;
    _masterChannel = 0;
    _secondaryMasterChannel = 0;
    _presenceMode = 0;
    _syncState = 0;
    _actionFrameTxMode = 0;
    _minRate = 0;
    _deviceCapabilities = 0;
    memcpy(_bssid, RTW88AWDL::BSSID, sizeof(_bssid));
    _peerRegistrations = 0;
    bzero(_syncChannelSequence, sizeof(_syncChannelSequence));
    _syncChannelSequenceLength = 0;
    _availabilityWindowLength = 0;
    _availabilityWindowPeriod = 0;
    _extensionLength = 0;
    _synchronizationFramePeriod = 0;
    _syncParamsValid = false;
    if (_syncTemplate) {
        IOFree(_syncTemplate, _syncTemplateLength);
        _syncTemplate = nullptr;
        _syncTemplateLength = 0;
    }
    });
}

void RTW88AWDLManager::setVirtualInterface(UInt role, IO80211VirtualInterface *interface)
{
    return awdlGated<void>(_workLoop, _owner, [&]() -> void {
    if (role == APPLE80211_VIF_AWDL) {
        _awdlInterface = interface;
        const uint64_t now = nowUS();
        _vifEnabledUS = now;
        _fallbackDeadlineUS = now + 1500000ULL;
        _appleControlMask |= kAppleCtlVIF;
        _lastAppleControlUS = now;
        /* AirportItlwm's useful lesson is lifecycle/control ownership: give
         * IO80211 a grace period to provide its AWDL template/sequence before
         * falling back to our OpenAWDL-derived bootstrap. */
        publishStats();
        arm(_airTemplate ? 1 : 25);
    }
    else if (role >= APPLE80211_VIF_P2P_DEVICE && role <= APPLE80211_VIF_P2P_GO)
        _p2pInterface = interface;
    });
}

void RTW88AWDLManager::clearVirtualInterface(IO80211VirtualInterface *interface)
{
    return awdlGated<void>(_workLoop, _owner, [&]() -> void {
    if (_awdlInterface == interface) {
        if (_backend) _backend->restoreSTAChannelAfterAWDL();
        _awdlInterface = nullptr;
        if (_timer) _timer->cancelTimeout();
        flushData();
        flushActions();
        _fallbackDeadlineUS = 0;
        publishStats();
    }
    if (_p2pInterface == interface)
        _p2pInterface = nullptr;
    });
}

void RTW88AWDLManager::noteAppleControl(uint32_t bit)
{
    awdlGated<void>(_workLoop, _owner, [&]() -> void {
        _appleControlMask |= bit;
        _lastAppleControlUS = nowUS();
        publishStats();
        arm(1);
    });
}

IOReturn RTW88AWDLManager::setSyncChannelSequence(const void *data, uint32_t length)
{
    return awdlGated<IOReturn>(_workLoop, _owner, [&]() -> IOReturn {
        if (!data || length == 0 || length > sizeof(_syncChannelSequence))
            return kIOReturnBadArgument;
        bzero(_syncChannelSequence, sizeof(_syncChannelSequence));
        memcpy(_syncChannelSequence, data, length);
        _syncChannelSequenceLength = length;
        _appleControlMask |= kAppleCtlChannelSequence;
        _lastAppleControlUS = nowUS();

        /* Ventura exposes fill_channel explicitly, while the packed seq flags
         * remain private/ambiguous in the pinned SDK. Preserve the entire ABI
         * blob for GET and use only the documented concrete hint; the radio is
         * never retuned from this IOCTL path. The AWDL scheduler owns retunes. */
        /* apple80211_awdl_sync_channel_sequence has fill_channel at byte 9
         * in the Ventura ABI (after version/pad/length/encoding/step/dup).
         * Read only that stable scalar and keep the sequence flags opaque. */
        if (length >= 10) {
            const uint8_t fillChannel = static_cast<const uint8_t *>(data)[9];
            /* In the AWDL channel-sequence format all-ones is a fill sentinel
             * (OpenAWDL uses 0xffff: repeat current channel). Ventura exposes
             * an 8-bit fill_channel in this private IOCTL ABI, so 0xff must
             * never escape as "AirDrop Channel 255". */
            if (isConcreteChannel(fillChannel)) {
                _masterChannel = fillChannel;
                if (_nativeSchedule) {
                    memset(_action.sequence.channel, fillChannel, sizeof(_action.sequence.channel));
                    _discoveryChannel = fillChannel;
                    _nextSocialSweepUS = 0;
                }
            }
        }
        publishStats();
        arm(1);
        return kIOReturnSuccess;
    });
}

void RTW88AWDLManager::setSyncParams(uint32_t awLen, uint32_t awPeriod,
                                      uint32_t extLen, uint32_t syncPeriod)
{
    awdlGated<void>(_workLoop, _owner, [&]() -> void {
        _availabilityWindowLength = awLen;
        _availabilityWindowPeriod = awPeriod;
        _extensionLength = extLen;
        _synchronizationFramePeriod = syncPeriod;
        _syncParamsValid = true;
        _appleControlMask |= kAppleCtlSyncParams;
        _lastAppleControlUS = nowUS();
        /* Do not reinterpret private units into OpenAWDL timing. The exact
         * values are retained for IO80211; the validated frame template is
         * authoritative for over-air timing. */
        publishStats();
        arm(1);
    });
}

void RTW88AWDLManager::setBSSID(const uint8_t *bssid)
{
    if (bssid) memcpy(_bssid, bssid, sizeof(_bssid));
}

void RTW88AWDLManager::getBSSID(uint8_t *bssid) const
{
    if (bssid) memcpy(bssid, _bssid, sizeof(_bssid));
}

void RTW88AWDLManager::setMasterChannel(uint32_t v)
{
    awdlGated<void>(_workLoop, _owner, [&]() -> void {
        _masterChannelRaw = v;
        /* 0xff is a private-ABI sentinel, not IEEE channel 255. Keep the last
         * concrete channel (or peer-observed fallback) rather than poisoning
         * System Information and the scheduler with the sentinel. */
        if (!isConcreteChannel(v)) {
            publishStats();
            return;
        }
        _masterChannel = v;
        /* OpenAWDL uses a static social-channel sequence when it owns the
         * schedule.  Our native fallback does the same, but prefer the
         * concrete social-channel hint supplied by IO80211 instead of
         * permanently assuming channel 6. */
        if (_nativeSchedule && (v == 6 || v == 44 || v == 149)) {
            memset(_action.sequence.channel, (uint8_t)v,
                   sizeof(_action.sequence.channel));
            _discoveryChannel = (uint8_t)v;
            _discoverySocialIndex = v == 6 ? 0 : v == 44 ? 1 : 2;
            _nextSocialSweepUS = 0;
            _lastMIFEAWValid = false;
            _nextPSFUS = 0;
            publishStats();
            arm(1);
        }
    });
}

IOReturn RTW88AWDLManager::setSyncFrameTemplate(const void *payload, uint32_t length, bool fromIO80211)
{
    return awdlGated<IOReturn>(_workLoop, _owner, [&]() -> IOReturn {
    RTW88AWDL::Action parsed;
    if (!RTW88AWDL::parseAction((const uint8_t *)payload, length, parsed)) {
        if (_owner) _owner->setProperty("AWDL_TEMPLATE_REJECTED_LENGTH", (uint64_t)length, 32);
        return kIOReturnBadArgument;
    }
    const uint32_t prefix = parsed.actionOffset ? 0 : 24;
    if (length + prefix > RTW88AWDL::MaxFrame) return kIOReturnNoSpace;
    if (!RTW88AWDL::unicast(_localAddress)) return kIOReturnNotReady;
    uint8_t *raw = (uint8_t *)IOMalloc(length);
    uint8_t *air = (uint8_t *)IOMalloc(length + prefix);
    if (!raw || !air) {
        if (raw) IOFree(raw, length);
        if (air) IOFree(air, length + prefix);
        return kIOReturnNoMemory;
    }
    memcpy(raw, payload, length);
    if (prefix) {
        bzero(air, 24); air[0] = 0xd0;
        memset(air + 4, 0xff, 6);
        memcpy(air + 16, RTW88AWDL::BSSID, 6);
    }
    memcpy(air + prefix, payload, length);
    memcpy(air + 10, _localAddress, 6);
    RTW88AWDL::Action normalized;
    RTW88AWDL::Clock clock;
    if (!RTW88AWDL::parseAction(air, length + prefix, normalized) ||
        !clock.set(normalized, nowUS())) {
        IOFree(raw, length); IOFree(air, length + prefix);
        return kIOReturnBadArgument;
    }
    if (_syncTemplate) IOFree(_syncTemplate, _syncTemplateLength);
    if (_airTemplate) IOFree(_airTemplate, _airTemplateLength);
    _syncTemplate = raw; _syncTemplateLength = length;
    _airTemplate = air; _airTemplateLength = length + prefix;
    _action = normalized; _clock = clock; _nextActionUS = 0; _nextPSFUS = 0;
    _lastMIFEAW = 0; _lastMIFEAWValid = false;
    _nativeSchedule = !fromIO80211; _masterSeenUS = 0;
    _fallbackDeadlineUS = 0;
    if (fromIO80211) {
        _appleControlMask |= kAppleCtlTemplate;
        _lastAppleControlUS = nowUS();
        _nextSocialSweepUS = 0;
    }
    if (_owner) _owner->setProperty("AWDL_TEMPLATE_READY", kOSBooleanTrue);
    publishStats();
    arm(1);
    return kIOReturnSuccess;
    });
}

IOReturn RTW88AWDLManager::copySyncFrameTemplate(void *payload, uint32_t *length) const
{
    return awdlGated<IOReturn>(_workLoop, _owner, [&]() -> IOReturn {
    if (!payload || !length)
        return kIOReturnBadArgument;
    if (!_syncTemplate || !_syncTemplateLength)
        return kIOReturnNotReady;

    /* Treat *length as the caller-provided capacity.  The old code copied the
     * whole saved template unconditionally, which could overflow an IO80211
     * buffer if a smaller payload was supplied on GET. */
    const uint32_t capacity = *length;
    *length = _syncTemplateLength;
    if (capacity < _syncTemplateLength)
        return kIOReturnNoSpace;

    memcpy(payload, _syncTemplate, _syncTemplateLength);
    return kIOReturnSuccess;
    });
}

void RTW88AWDLManager::notePeerTrafficRegistration(bool active)
{
    if (active)
        ++_peerRegistrations;
    else if (_peerRegistrations)
        --_peerRegistrations;
}

uint64_t RTW88AWDLManager::nowUS()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}

void RTW88AWDLManager::arm(uint32_t milliseconds)
{
    if (_timer && _syncEnabled && _awdlInterface)
        _timer->setTimeoutMS(milliseconds);
}

void RTW88AWDLManager::setSyncEnabled(bool enabled)
{
    return awdlGated<void>(_workLoop, _owner, [&]() -> void {
    _syncEnabled = enabled;
    publishStats();
    if (!enabled) {
        if (_backend) _backend->restoreSTAChannelAfterAWDL();
        if (_timer) _timer->cancelTimeout();
        flushData();
        flushActions();
    } else {
        const uint64_t now = nowUS();
        if (!_airTemplate) _fallbackDeadlineUS = now + 1500000ULL;
        _nextActionUS = _nextPSFUS = 0;
        _lastMIFEAW = 0; _lastMIFEAWValid = false;
        arm(_airTemplate ? 1 : 25);
    }
    });
}

void RTW88AWDLManager::setLocalAddress(const uint8_t *mac)
{
    return awdlGated<void>(_workLoop, _owner, [&]() -> void {
    if (!mac || !RTW88AWDL::unicast(mac)) return;
    memcpy(_localAddress, mac, 6);
    if (_backend) _backend->setAWDLAddress(mac);
    if (_airTemplate) memcpy(_airTemplate + 10, mac, 6);
    if (_nativeSchedule) {
        memcpy(_action.master,mac,6);memcpy(_action.syncAddress,mac,6);
        _action.height=0;_action.masterCounter=0;_action.masterMetric=60;_masterSeenUS=0;
    }
    if (!_airTemplate && _awdlInterface) {
        const uint64_t now = nowUS();
        if (!_fallbackDeadlineUS) _fallbackDeadlineUS = now + 1500000ULL;
        arm(25);
    }
    });
}

void RTW88AWDLManager::bootstrapNative()
{
    if (_airTemplate || !_awdlInterface || !_syncEnabled || !RTW88AWDL::unicast(_localAddress)) return;
    RTW88AWDL::Action initial;
    initial.awPeriod=16;initial.afPeriod=110;initial.commonLength=16;initial.presence=4;
    initial.sequence.count=16;initial.sequence.stride=4;
    const uint8_t social = (_masterChannel == 6 || _masterChannel == 44 || _masterChannel == 149)
        ? (uint8_t)_masterChannel : 6;
    memset(initial.sequence.channel,social,sizeof(initial.sequence.channel));
    _discoveryChannel = social;
    _discoverySocialIndex = social == 6 ? 0 : social == 44 ? 1 : 2;
    _nextSocialSweepUS = 0;
    memcpy(initial.master,_localAddress,6);memcpy(initial.syncAddress,_localAddress,6);
    initial.masterMetric=60;
    uint8_t frame[RTW88AWDL::NativeFrameSize];
    const size_t length=RTW88AWDL::buildNativeAction(frame,sizeof(frame),_localAddress,initial,0);
    if (length && setSyncFrameTemplate(frame,(uint32_t)length,false)==kIOReturnSuccess) {
        _nextPSFUS=0; _lastMIFEAW=0; _lastMIFEAWValid=false;
        publishStats();
    }
}

void RTW88AWDLManager::noteDataRX(uint32_t result)
{
    awdlGated<void>(_workLoop,_owner,[&]() -> void {
        ++_dataRx;_lastDataRxResult=result;
    });
}

void RTW88AWDLManager::flushActions()
{
    for (auto &entry : _pendingActions) {
        if (entry.packet) { mbuf_freem(entry.packet); ++_appleActionDropped; }
        entry.packet = nullptr; entry.queuedUS = 0;
    }
    _actionQueueHead = _actionQueueCount = 0;
}

bool RTW88AWDLManager::enqueueActionFrame(mbuf_t m)
{
    return awdlGated<bool>(_workLoop, _owner, [&]() -> bool {
        if (!m) return false;
        const size_t len = mbuf_pkthdr_len(m);
        uint8_t fc[2] = {};
        const bool management = len >= 24 && len <= RTW88AWDL::MaxFrame &&
            mbuf_copydata(m, 0, sizeof(fc), fc) == 0 && ((fc[0] & 0x0c) == 0);
        if (!_syncEnabled || !_awdlInterface || !management || _actionQueueCount == 32) {
            mbuf_freem(m); ++_appleActionDropped;
            publishStats();
            return false;
        }

        /* Some Ventura paths deliver the concrete Apple AWDL action through
         * BPF instead of (or before) AWDL_SYNC_FRAME_TEMPLATE. If it parses as
         * an AWDL action, adopt it as the authoritative Apple template so its
         * own channel sequence/timing drives the Realtek timeslicer. */
        if (!_airTemplate && _txBuffer &&
            mbuf_copydata(m, 0, len, _txBuffer) == 0) {
            RTW88AWDL::Action appleAction;
            if (RTW88AWDL::parseAction(_txBuffer, (uint32_t)len, appleAction) &&
                setSyncFrameTemplate(_txBuffer, (uint32_t)len, true) == kIOReturnSuccess)
                ++_bpfTemplateAdoptions;
        }

        const uint64_t queuedAt = nowUS();
        if (!_airTemplate && (!_fallbackDeadlineUS || _fallbackDeadlineUS > queuedAt + 100000ULL))
            _fallbackDeadlineUS = queuedAt + 100000ULL;
        PendingAction &entry = _pendingActions[(_actionQueueHead + _actionQueueCount) % 32];
        entry.packet = m; entry.queuedUS = queuedAt;
        ++_actionQueueCount; ++_appleActionQueued;
        arm(1);
        return true;
    });
}

void RTW88AWDLManager::drainActions(uint64_t now)
{
    unsigned budget = 8;
    while (_actionQueueCount && budget--) {
        PendingAction entry = _pendingActions[_actionQueueHead];
        _pendingActions[_actionQueueHead] = {};
        _actionQueueHead = uint8_t((_actionQueueHead + 1) % 32);
        --_actionQueueCount;
        const size_t len = entry.packet ? mbuf_pkthdr_len(entry.packet) : 0;
        const bool stale = !entry.packet || now < entry.queuedUS || now - entry.queuedUS > 750000ULL;
        if (stale || len < 24 || len > RTW88AWDL::MaxFrame ||
            mbuf_copydata(entry.packet, 0, len, _txBuffer) != 0) {
            if (entry.packet) mbuf_freem(entry.packet);
            ++_appleActionDropped;
            continue;
        }
        /* BPF can enqueue an Apple-generated AWDL action several hundred
         * milliseconds before the single PHY gets an AWDL window. Its timing
         * fields are then stale. When IO80211 owns the schedule, re-stamp only
         * those parsed actions that match the authoritative Apple schedule;
         * never reinterpret them using the native fallback schedule. */
        if (!_nativeSchedule) {
            RTW88AWDL::Action queuedAction;
            if (RTW88AWDL::parseAction(_txBuffer, len, queuedAction) &&
                queuedAction.actionOffset == 24 &&
                queuedAction.awPeriod == _action.awPeriod &&
                queuedAction.presence == _action.presence) {
                const auto sendWindow = RTW88AWDL::window(_clock, _action.sequence, now);
                if (sendWindow.channel) {
                    RTW88AWDL::stamp(_txBuffer, queuedAction, sendWindow, now, _actionSequence++);
                    ++_appleActionRestamped;
                }
            }
        }
        if (_backend->txRawManagementFrame(_txBuffer, (uint32_t)len)) {
            ++_appleActionTx; ++_actionTx;
        } else {
            ++_appleActionDropped;
        }
        mbuf_freem(entry.packet);
    }
}

void RTW88AWDLManager::flushData()
{
    for (auto &entry : _pending) {
        if (entry.packet) { mbuf_freem(entry.packet); ++_dataDropped; }
        entry.packet = nullptr; entry.queuedUS = 0;
    }
    _queueHead = _queueCount = 0;
}

bool RTW88AWDLManager::enqueueData(mbuf_t m)
{
    return awdlGated<bool>(_workLoop, _owner, [&]() -> bool {
    if (!m) return false;
    if (!_syncEnabled || !_awdlInterface || !_airTemplate || _queueCount == 128 ||
        mbuf_pkthdr_len(m) < 14 || mbuf_pkthdr_len(m) > 4096) {
        mbuf_freem(m); ++_dataDropped;
        return false;
    }
    Pending &entry = _pending[(_queueHead + _queueCount) % 128];
    entry.packet = m; entry.queuedUS = nowUS();
    ++_queueCount;
    arm(1);
    return true;
    });
}

bool RTW88AWDLManager::observeAction(const uint8_t *frame, uint32_t length, bool *presenceDue)
{
    return awdlGated<bool>(_workLoop, _owner, [&]() -> bool {
    if (presenceDue) *presenceDue = false;
    if (!frame || length < 40) return false;
    if (!_awdlInterface || !memcmp(frame + 10, _localAddress, 6)) return false;
    ++_actionCandidates;
    const uint64_t now = nowUS();
    RTW88AWDL::Action action;
    RTW88AWDL::ParseFailure failure;
    if (!RTW88AWDL::parseAction(frame, length, action, &failure) || action.actionOffset != 24) {
        ++_actionParseRejected;
        // One bounded management-frame sample per second, never data traffic.
        // Preserve the bytes rather than guessing which Apple TLV differed.
        if (_owner && length <= RTW88AWDL::MaxFrame &&
            (_actionParseRejected == 1 || now - _lastRejectReportUS >= 1000000)) {
            _owner->setProperty("AWDL_REJECT_REASON", failure.reason);
            _owner->setProperty("AWDL_REJECT_OFFSET", (uint64_t)failure.offset, 32);
            // IORegistry's legacy byte-copy overload lacks const qualification.
            _owner->setProperty("AWDL_REJECT_FRAME", const_cast<uint8_t *>(frame), length);
            _lastRejectReportUS = now;
        }
        return false;
    }
    RTW88AWDL::Clock clock;
    if (!clock.set(action, now)) { ++_actionClockRejected; return false; }
    Peer *peer = nullptr, *oldest = &_peers[0];
    for (auto &p : _peers) {
        if (!memcmp(p.mac, frame + 10, 6)) { peer = &p; break; }
        if (p.seenUS < oldest->seenUS) oldest = &p;
    }
    if (!peer) peer = oldest;
    memcpy(peer->mac, frame + 10, 6);
    peer->clock = clock; peer->sequence = action.sequence; peer->seenUS = now;
    /* OpenAWDL only flips the peer's fully-valid lifecycle state after MIF +
     * Version TLV + device class, but it creates/uses the peer earlier for
     * synchronization, election and channel tracking. Keep those concepts
     * separate here as well. */
    if (action.subtype == 3) {
        if (!peer->sawMIF) ++_mifPeersObserved;
        peer->sawMIF = true;
        ++_mifRx;
    }
    if (action.versionValid) {
        if (!peer->versionValid) ++_versionPeersObserved;
        peer->versionValid = true; peer->version = action.version;
        peer->deviceClass = action.deviceClass;
        ++_versionRx;
    }
    /* Keep OpenAWDL's full peer-promotion rule for IO80211 publication, but
     * do not confuse that lifecycle bit with whether the peer may take part
     * in synchronization/election. OWL creates the peer on the first valid
     * action frame and only flips is_valid after MIF+Version+devclass. */
    peer->valid = peer->sawMIF && peer->versionValid &&
                  peer->version != 0 && peer->deviceClass != 0;
    _lastActionRxUS = now;
    if (isConcreteChannel(action.masterChannel))
        _observedMasterChannel = action.masterChannel;
    if (isConcreteChannel(action.nextAwChannel))
        _nextAWChannel = action.nextAwChannel;
    ++_actionRx;
    // The fallback follows an advertised election-v2 master, never a node
    // that already synchronizes to us. Preserve an Apple-supplied schedule.
    if (_nativeSchedule && action.electionValid && action.height<10 &&
        peer->seenUS && memcmp(action.master,_localAddress,6) &&
        memcmp(action.syncAddress,_localAddress,6)) {
        const bool current=!memcmp(_action.syncAddress,frame+10,6);
        const bool expired=!_masterSeenUS || now-_masterSeenUS>3000000;
        /* Match OpenAWDL's election ordering: top-master counter, then metric;
         * if both are equal prefer the shallower sync tree, then the larger
         * synchronization-peer MAC as the deterministic tie breaker. */
        const uint32_t currentPeerHeight = _action.height ? _action.height - 1 : 0;
        bool better = action.masterCounter > _action.masterCounter;
        if (action.masterCounter == _action.masterCounter) {
            if (action.masterMetric > _action.masterMetric) better = true;
            else if (action.masterMetric == _action.masterMetric) {
                if (action.height < currentPeerHeight) better = true;
                else if (action.height == currentPeerHeight &&
                         memcmp(frame+10,_action.syncAddress,6)>0) better = true;
            }
        }
        if (current || expired || better) {
            _clock=clock;
            memcpy(_action.master,action.master,6);memcpy(_action.syncAddress,frame+10,6);
            _action.masterCounter=action.masterCounter;_action.masterMetric=action.masterMetric;
            _action.height=action.height+1;
            _action.awPeriod=action.awPeriod;_action.presence=action.presence;
            _action.commonLength=action.commonLength;_action.sequence=action.sequence;
            _masterSeenUS=now;++_syncUpdates;
        }
    }
    // IO80211 owns election. Only follow the master named in its template,
    // never let an unrelated nearby peer reset our clock.
    if (!_nativeSchedule && _airTemplate && !memcmp(_action.master, frame + 10, 6) &&
        action.awPeriod == _action.awPeriod && action.presence == _action.presence) {
        _clock = clock;
        ++_syncUpdates;
    }
    if (peer->valid && presenceDue) {
        /* IO80211 peer presence is a lifecycle event, not a per-frame trace.
         * Publish on promotion and at most once per second thereafter so the
         * peer manager can refresh RSSI/channel without being flooded by every
         * PSF/MIF. */
        if (!peer->announced || !peer->lastPresenceUS ||
            now < peer->lastPresenceUS || now - peer->lastPresenceUS >= 1000000ULL) {
            *presenceDue = true;
            peer->announced = true;
            peer->lastPresenceUS = now;
        }
    }
    return peer->valid;
    });
}

uint32_t RTW88AWDLManager::expirePublishedPeers(uint8_t *outMacs, uint32_t capacity)
{
    return awdlGated<uint32_t>(_workLoop, _owner, [&]() -> uint32_t {
        if (!outMacs || !capacity) return 0;
        const uint64_t now = nowUS();
        uint32_t count = 0;
        for (auto &peer : _peers) {
            if (!peer.announced || !peer.seenUS || now < peer.seenUS ||
                now - peer.seenUS <= 3000000ULL)
                continue;
            if (count < capacity) {
                memcpy(outMacs + count * 6, peer.mac, 6);
                ++count;
            }
            /* Do not leave stale peer state in IO80211. A future MIF/version
             * observation will promote this MAC again and generate a fresh
             * presence event. */
            peer.announced = false;
            peer.lastPresenceUS = 0;
            peer.valid = false;
            peer.sawMIF = false;
            peer.versionValid = false;
            peer.version = 0;
            peer.deviceClass = 0;
        }
        return count;
    });
}

void RTW88AWDLManager::drainData(uint64_t now, const RTW88AWDL::Window &window)
{
    const unsigned count = _queueCount;
    unsigned submitted = 0;
    for (unsigned i = 0; i < count; ++i) {
        Pending entry = _pending[_queueHead];
        _pending[_queueHead] = {};
        _queueHead = (_queueHead + 1) % 128;
        --_queueCount;
        uint8_t destination[6] = {};
        bool expired = now < entry.queuedUS || now - entry.queuedUS > 2000000;
        if (expired || mbuf_copydata(entry.packet, 0, 6, destination) != 0) {
            mbuf_freem(entry.packet); ++_dataDropped;
            continue;
        }
        bool ready = false;
        if (destination[0] & 1) {
            ready = RTW88AWDL::multicastWindow(window);
        } else {
            for (const auto &peer : _peers) {
                /* OpenAWDL allows a parsed peer to participate in the data
                 * path before the peer-valid callback fires; validity is a
                 * publication/lifecycle state, not a prerequisite for using
                 * the peer's clock and channel sequence. */
                if (!peer.seenUS || now < peer.seenUS ||
                    now - peer.seenUS > 3000000 || memcmp(destination, peer.mac, 6)) continue;
                const auto pw = RTW88AWDL::window(peer.clock, peer.sequence, now);
                ready = pw.channel == window.channel && RTW88AWDL::insideGuard(pw);
                break;
            }
        }
        if (ready && submitted < 16 && _backend->canTransmitAWDL()) {
            if (_backend->txAWDLDataFrame(entry.packet)) ++_dataTx;
            else ++_dataDropped;
            ++submitted;
        } else {
            _pending[(_queueHead + _queueCount) % 128] = entry;
            ++_queueCount;
        }
    }
}

void RTW88AWDLManager::publishStats()
{
    if (!_owner) return;
    const char *plane = !_awdlInterface ? "no-interface" :
        !_syncEnabled ? "sync-disabled" :
        !_airTemplate ? "waiting-io80211" :
        !_nativeSchedule ? "io80211-template" :
        (_appleControlMask & ~kAppleCtlVIF) ? "hybrid-bootstrap" : "driver-bootstrap";
    _owner->setProperty("AWDL_SCHEDULER_STATE", plane);
    _owner->setProperty("AWDL_CONTROL_PLANE", plane);
    _owner->setProperty("AWDL_SCHEDULER_VERSION", "2.0.0-itlwm-control-openawdl-radio");
    _owner->setProperty("AWDL_OPENAWDL_ALIGNMENT", "opclass-mif-ht-election-restamp");
    _owner->setProperty("AWDL_APPLE_CONTROL_MASK", (uint64_t)_appleControlMask, 32);
    _owner->setProperty("AWDL_IO80211_CONTROL_SEEN", (uint64_t)((_appleControlMask & ~kAppleCtlVIF) != 0), 8);
    _owner->setProperty("AWDL_ACTION_CANDIDATES", (uint64_t)_actionCandidates, 32);
    _owner->setProperty("AWDL_ACTION_PARSE_REJECTED", (uint64_t)_actionParseRejected, 32);
    _owner->setProperty("AWDL_ACTION_CLOCK_REJECTED", (uint64_t)_actionClockRejected, 32);
    _owner->setProperty("AWDL_TEMPLATE_SOURCE",_nativeSchedule ? "driver" : "IO80211");
    _owner->setProperty("AWDL_DATA_RX",(uint64_t)_dataRx,32);
    _owner->setProperty("AWDL_DATA_RX_RESULT",(uint64_t)_lastDataRxResult,32);
    _owner->setProperty("AWDL_ELECTION_HEIGHT",(uint64_t)_action.height,32);
    _owner->setProperty("AWDL_ACTION_TX", (uint64_t)_actionTx, 32);
    _owner->setProperty("AWDL_APPLE_AF_QUEUED", (uint64_t)_appleActionQueued, 32);
    _owner->setProperty("AWDL_APPLE_AF_TX", (uint64_t)_appleActionTx, 32);
    _owner->setProperty("AWDL_APPLE_AF_DROPPED", (uint64_t)_appleActionDropped, 32);
    _owner->setProperty("AWDL_APPLE_AF_RESTAMPED", (uint64_t)_appleActionRestamped, 32);
    _owner->setProperty("AWDL_APPLE_AF_QUEUE_DEPTH", (uint64_t)_actionQueueCount, 32);
    _owner->setProperty("AWDL_BPF_TEMPLATE_ADOPTED", (uint64_t)_bpfTemplateAdoptions, 32);
    _owner->setProperty("AWDL_PSF_TX", (uint64_t)_psfTx, 32);
    _owner->setProperty("AWDL_MIF_TX", (uint64_t)_mifTx, 32);
    uint32_t validPeers = 0, observedPeers = 0, mifPeers = 0, versionPeers = 0;
    const uint64_t peerNow = nowUS();
    for (const auto &peer : _peers) {
        if (!peer.seenUS || peerNow < peer.seenUS || peerNow - peer.seenUS > 3000000)
            continue;
        ++observedPeers;
        if (peer.sawMIF) ++mifPeers;
        if (peer.versionValid) ++versionPeers;
        if (peer.valid) ++validPeers;
    }
    _owner->setProperty("AWDL_OBSERVED_PEERS", (uint64_t)observedPeers, 32);
    _owner->setProperty("AWDL_MIF_PEERS", (uint64_t)mifPeers, 32);
    _owner->setProperty("AWDL_VERSION_PEERS", (uint64_t)versionPeers, 32);
    _owner->setProperty("AWDL_VALID_PEERS", (uint64_t)validPeers, 32);
    _owner->setProperty("AWDL_MIF_RX", (uint64_t)_mifRx, 32);
    _owner->setProperty("AWDL_VERSION_RX", (uint64_t)_versionRx, 32);
    _owner->setProperty("AWDL_MIF_PEERS_EVER", (uint64_t)_mifPeersObserved, 32);
    _owner->setProperty("AWDL_VERSION_PEERS_EVER", (uint64_t)_versionPeersObserved, 32);
    _owner->setProperty("AWDL_ACTION_RX", (uint64_t)_actionRx, 32);
    _owner->setProperty("AWDL_SYNC_UPDATES", (uint64_t)_syncUpdates, 32);
    _owner->setProperty("AWDL_DATA_TX", (uint64_t)_dataTx, 32);
    _owner->setProperty("AWDL_DATA_DROPPED", (uint64_t)_dataDropped, 32);
    _owner->setProperty("AWDL_QUEUE_DEPTH", (uint64_t)_queueCount, 32);
    _owner->setProperty("AWDL_BLOCKED_WINDOWS", (uint64_t)_blockedWindows, 32);
    _owner->setProperty("AWDL_WINDOW_SKIPS", (uint64_t)_windowSkips, 32);
    _owner->setProperty("AWDL_DISCOVERY_CHANNEL", (uint64_t)_discoveryChannel, 32);
    _owner->setProperty("AWDL_SOCIAL_SWEEPS", (uint64_t)_socialSweeps, 32);
    _owner->setProperty("AWDL_MASTER_CHANNEL_RAW", (uint64_t)_masterChannelRaw, 32);
    _owner->setProperty("AWDL_MASTER_CHANNEL_HINT", (uint64_t)_masterChannel, 32);
    _owner->setProperty("AWDL_OBSERVED_MASTER_CHANNEL", (uint64_t)_observedMasterChannel, 32);
    _owner->setProperty("AWDL_NEXT_AW_CHANNEL", (uint64_t)_nextAWChannel, 32);
    _owner->setProperty("AWDL_REPORTED_CHANNEL", (uint64_t)reportedChannel(), 32);
    _owner->setProperty("AWDL_CHANNEL_RESULT", (uint64_t)(uint32_t)_lastChannelResult, 32);
}

bool RTW88AWDLManager::tick()
{
    return awdlGated<bool>(_workLoop, _owner, [&]() -> bool {
    if (!_backend || !_syncEnabled || !_awdlInterface || !_txBuffer) return false;
    uint64_t now = nowUS();
    if (!_airTemplate) {
        if (!_fallbackDeadlineUS) _fallbackDeadlineUS = now + 1500000ULL;
        if (now >= _fallbackDeadlineUS) bootstrapNative();
        if (!_airTemplate) {
            if (now >= _lastStatsUS + 250000ULL) { publishStats(); _lastStatsUS = now; }
            arm(25);
            return false;
        }
        now = nowUS();
    }
    if (_nativeSchedule && _masterSeenUS && now-_masterSeenUS>3000000) {
        memcpy(_action.master,_localAddress,6);memcpy(_action.syncAddress,_localAddress,6);
        _action.height=0;_action.masterCounter=0;_action.masterMetric=60;
        _action.awPeriod=16;_action.presence=4;_action.commonLength=16;
        _action.sequence.count=16;_action.sequence.stride=4;
        const uint8_t fallbackSocial = (_discoveryChannel==6 || _discoveryChannel==44 || _discoveryChannel==149)
            ? _discoveryChannel : ((_masterChannel==6 || _masterChannel==44 || _masterChannel==149)
                                  ? (uint8_t)_masterChannel : 6);
        memset(_action.sequence.channel,fallbackSocial,sizeof(_action.sequence.channel));
        _discoveryChannel=fallbackSocial;
        _discoverySocialIndex=fallbackSocial==6 ? 0 : fallbackSocial==44 ? 1 : 2;
        _clock.epochUS=now;_clock.epochAW=0;_clock.periodTU=16;_clock.presence=4;
        _masterSeenUS=0;_nextSocialSweepUS=0;_lastMIFEAWValid=false;_nextPSFUS=0;
    }
    /* Sweep only while there is no recent over-the-air AWDL evidence.
     * Requiring a fully promoted peer here is a deadlock: MIF/Version may
     * need multiple frames on one social channel, but the old loop hopped
     * away every 220 ms until that promotion had already completed. Freeze
     * as soon as a parsed Apple action is flowing and resume discovery after
     * a quiet interval. This restores the v57 behavior without permanently
     * pinning the radio after a stale peer disappears. */
    const bool recentAction = _lastActionRxUS && now >= _lastActionRxUS &&
                              now - _lastActionRxUS <= 750000ULL;
    if (_nativeSchedule && !_masterSeenUS && !recentAction) {
        static const uint8_t socials[3] = { 6, 44, 149 };
        if (!_nextSocialSweepUS)
            _nextSocialSweepUS = now + 220000ULL;
        else if (now >= _nextSocialSweepUS) {
            _discoverySocialIndex = uint8_t((_discoverySocialIndex + 1) % 3);
            _discoveryChannel = socials[_discoverySocialIndex];
            memset(_action.sequence.channel, _discoveryChannel, sizeof(_action.sequence.channel));
            _lastMIFEAWValid = false;
            _nextPSFUS = 0;
            ++_socialSweeps;
            _nextSocialSweepUS = now + 220000ULL;
        }
    } else {
        _nextSocialSweepUS = 0;
        uint8_t peerChannel = 0;
        if (isConcreteChannel(_observedMasterChannel))
            peerChannel = _observedMasterChannel;
        else if (isConcreteChannel(_nextAWChannel))
            peerChannel = _nextAWChannel;
        else if (_masterSeenUS && _action.sequence.count)
            peerChannel = _action.sequence.channel[0];
        if (peerChannel == 6 || peerChannel == 44 || peerChannel == 149) {
            if (_discoveryChannel != peerChannel && !_masterSeenUS) {
                _discoveryChannel = peerChannel;
                _discoverySocialIndex = peerChannel == 6 ? 0 : peerChannel == 44 ? 1 : 2;
                memset(_action.sequence.channel, peerChannel, sizeof(_action.sequence.channel));
                _lastMIFEAWValid = false;
                _nextPSFUS = 0;
            } else {
                _discoveryChannel = peerChannel;
            }
        }
    }

    auto window = RTW88AWDL::window(_clock, _action.sequence, now);

    /* One RTL88 PHY must time-share infrastructure STA and AWDL. Apple AWDL
     * advertises a common availability window inside the larger EAW. When
     * that channel differs from the AP, spend only the common window away
     * from infrastructure and return home for the rest of the EAW. */
    const bool sta = _backend->staAssociatedForAWDL();
    const uint16_t homeChannel = sta ? _backend->infrastructureChannel() : 0;
    const bool offChannelWanted = sta && window.channel && homeChannel &&
                                  window.channel != homeChannel;
    const uint64_t commonUS = uint64_t(_action.commonLength ?
                                       _action.commonLength : _action.awPeriod) * 1024ULL;
    const uint64_t guardUS = 3ULL * 1024ULL;
    const bool inCommonWindow = !offChannelWanted ||
        (commonUS > guardUS * 2 && window.elapsedUS >= guardUS &&
         window.elapsedUS + guardUS < commonUS);

    if (offChannelWanted && !inCommonWindow) {
        _backend->restoreSTAChannelAfterAWDL();
        /* Not being inside an AWDL common window is normal scheduling, not
         * a channel-switch failure.  v55 reported kIOReturnBusy here, making
         * AWDL_CHANNEL_RESULT look broken even while the scheduler was simply
         * keeping the PHY on the infrastructure AP. */
        _lastChannelResult = kIOReturnSuccess;
    } else {
        _lastChannelResult = window.channel ?
            _backend->setAWDLChannel(window.channel) : kIOReturnNotReady;
    }

    // A channel hop may cross the edge of the common window. Never send using
    // the timestamp/channel decision made before the retune completed.
    now = nowUS();
    auto after = RTW88AWDL::window(_clock, _action.sequence, now);
    const bool afterCommon = !offChannelWanted ||
        (commonUS > guardUS * 2 && after.elapsedUS >= guardUS &&
         after.elapsedUS + guardUS < commonUS);
    bool ready = _lastChannelResult == kIOReturnSuccess && after.slot == window.slot &&
        RTW88AWDL::insideGuard(after) && afterCommon && _backend->canTransmitAWDL();
    if (!ready && offChannelWanted && _backend->staTxBlockedByAWDL())
        _backend->restoreSTAChannelAfterAWDL();
    if (ready) {
        /* IO80211/BPF-generated management frames are authoritative control
         * traffic. Queue them from bpfOutputPacket and emit them only while
         * the single RTL PHY actually owns an AWDL availability window. */
        drainActions(now);
        if (_nativeSchedule) {
            /* OWL/OpenAWDL uses independent action-frame cadences: PSFs are
             * periodic, while one MIF is emitted per EAW.  A single-PHY Mac
             * cannot stay away from its infrastructure AP for the entire EAW,
             * so place that MIF near the center of the common window we
             * actually own instead of alternating PSF/MIF every afPeriod. */
            const uint64_t commonCenterUS = (uint64_t(_action.commonLength) * 1024) / 2;
            const uint32_t eaw = _action.presence ? uint32_t(after.aw / _action.presence) : 0;
            const bool mifDue = (!_lastMIFEAWValid || eaw != _lastMIFEAW) &&
                after.elapsedUS >= commonCenterUS;
            if (mifDue) {
                uint32_t frameLength=(uint32_t)RTW88AWDL::buildNativeAction(
                    _txBuffer,RTW88AWDL::MaxFrame,_localAddress,_action,3);
                RTW88AWDL::stamp(_txBuffer,_action,after,now,_actionSequence++);
                if (frameLength && _backend->txRawManagementFrame(_txBuffer,frameLength)) {
                    ++_actionTx; ++_mifTx;
                    _lastMIFEAW=eaw; _lastMIFEAWValid=true;
                }
            }
            if (now >= _nextPSFUS) {
                uint32_t frameLength=(uint32_t)RTW88AWDL::buildNativeAction(
                    _txBuffer,RTW88AWDL::MaxFrame,_localAddress,_action,0);
                RTW88AWDL::stamp(_txBuffer,_action,after,now,_actionSequence++);
                if (frameLength && _backend->txRawManagementFrame(_txBuffer,frameLength)) {
                    ++_actionTx; ++_psfTx;
                    _nextPSFUS = now + uint64_t(_action.afPeriod) * 1024;
                }
            }
        } else if (now >= _nextActionUS) {
            memcpy(_txBuffer, _airTemplate, _airTemplateLength);
            RTW88AWDL::stamp(_txBuffer, _action, after, now, _actionSequence++);
            if (_airTemplateLength &&
                _backend->txRawManagementFrame(_txBuffer, _airTemplateLength)) ++_actionTx;
            _nextActionUS = now + uint64_t(_action.afPeriod) * 1024;
        }
        drainData(now, after);
    } else {
        if (_actionQueueCount) {
            PendingAction &head = _pendingActions[_actionQueueHead];
            if (head.packet && now > head.queuedUS + 750000ULL) {
                mbuf_freem(head.packet); head = {};
                _actionQueueHead = uint8_t((_actionQueueHead + 1) % 32);
                --_actionQueueCount; ++_appleActionDropped;
            }
        }
        if (inCommonWindow) ++_blockedWindows;
        else ++_windowSkips;
        // Expire old packets even while STA owns the radio.
        for (auto &entry : _pending) {
            if (entry.packet && now > entry.queuedUS + 2000000) {
                // Keep queue invariants: drainData expires without transmitting
                // with a zero channel and a non-multicast slot.
                RTW88AWDL::Window unavailable; unavailable.slot = 1;
                drainData(now, unavailable);
                break;
            }
        }
    }
    if (now >= _lastStatsUS + 1000000) { publishStats(); _lastStatsUS = now; }
    /* Connected coexistence needs sub-window resolution: a 16 ms retry can
     * miss an entire 16-TU common window. */
    arm(sta ? 4 : (_lastChannelResult == kIOReturnSuccess ? 2 : 16));
    return ready;
    });
}
