/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * RTW88AWDLManager.hpp — shared AWDL/P2P state for AirPort_RTW88 1.0.1.
 *
 * This class intentionally contains no private Apple ABI layouts. It owns the
 * verified AWDL state that is shared between IO80211 virtual-interface IOCTLs
 * and the Realtek management-frame transport.
 */
#pragma once

#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOReturn.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOService.h>
#include <sys/kpi_mbuf.h>
#include "RTW88AWDLProtocol.hpp"


class RTW88IEEE80211;
class IO80211VirtualInterface;

class RTW88AWDLManager {
public:
    /* IO80211 owns AWDL policy/control.  These bits record which parts of the
     * Ventura control-plane macOS has actually supplied; OpenAWDL-style
     * fallback is only a bootstrap when Apple has not provided a template. */
    enum AppleControl : uint32_t {
        kAppleCtlVIF             = 1u << 0,
        kAppleCtlSyncEnabled     = 1u << 1,
        kAppleCtlTemplate        = 1u << 2,
        kAppleCtlMasterChannel   = 1u << 3,
        kAppleCtlChannelSequence = 1u << 4,
        kAppleCtlSyncParams      = 1u << 5,
        kAppleCtlPresence        = 1u << 6,
        kAppleCtlSyncState       = 1u << 7,
        kAppleCtlElection        = 1u << 8,
    };

    RTW88AWDLManager() = default;
    ~RTW88AWDLManager();

    bool init(RTW88IEEE80211 *backend, IOWorkLoop *workLoop,
              IOService *owner, IOTimerEventSource::Action action);
    void reset();
    void scheduleDiscovery();
    void suspendForPowerTransition();
    void resumeAfterPowerTransition();

    void setVirtualInterface(UInt role, IO80211VirtualInterface *interface);
    void clearVirtualInterface(IO80211VirtualInterface *interface);
    IO80211VirtualInterface *awdlInterface() const { return _awdlInterface; }
    IO80211VirtualInterface *p2pInterface() const { return _p2pInterface; }

    bool syncEnabled() const { return _syncEnabled; }
    void setSyncEnabled(bool enabled);
    void noteAppleControl(uint32_t bit);
    uint32_t appleControlMask() const { return _appleControlMask; }
    void setLocalAddress(const uint8_t *mac);
    bool observeAction(const uint8_t *frame, uint32_t length, bool *presenceDue = nullptr);
    uint32_t expirePublishedPeers(uint8_t *outMacs, uint32_t capacity);
    bool enqueueActionFrame(mbuf_t m); // consumes the mbuf on every path
    bool enqueueData(mbuf_t m); // consumes the mbuf on every path
    bool tick(); // true when the controller should pull more packets
    bool scheduleReady() const { return _syncEnabled && _awdlInterface && _airTemplate; }
    void publishStats();
    void noteDataRX(uint32_t result);

    uint32_t electionMetric() const { return _electionMetric; }
    void setElectionMetric(uint32_t metric) { _electionMetric = metric; }

    void setBSSID(const uint8_t *bssid);
    void getBSSID(uint8_t *bssid) const;
    uint32_t electionId() const { return _electionId; }
    void setElectionId(uint32_t v) { _electionId = v; }
    uint32_t masterChannel() const { return _masterChannel; }
    uint32_t reportedChannel() const {
        if (isConcreteChannel(_masterChannel)) return _masterChannel;
        if (isConcreteChannel(_observedMasterChannel)) return _observedMasterChannel;
        if (isConcreteChannel(_discoveryChannel)) return _discoveryChannel;
        return 6;
    }
    void setMasterChannel(uint32_t v);
    uint32_t secondaryMasterChannel() const { return _secondaryMasterChannel; }
    void setSecondaryMasterChannel(uint32_t v) { _secondaryMasterChannel = v; }
    uint8_t minRate() const { return _minRate; }
    void setMinRate(uint8_t v) { _minRate = v; }
    uint32_t presenceMode() const { return _presenceMode; }
    void setPresenceMode(uint32_t v) { _presenceMode = v; }
    uint32_t syncState() const { return _syncState; }
    void setSyncState(uint32_t v) { _syncState = v; }
    uint8_t deviceCapabilities() const { return _deviceCapabilities; }
    void setDeviceCapabilities(uint8_t v) { _deviceCapabilities = v; }
    uint64_t actionFrameTxMode() const { return _actionFrameTxMode; }
    void setActionFrameTxMode(uint64_t v) { _actionFrameTxMode = v; }

    IOReturn setSyncChannelSequence(const void *data, uint32_t length);
    IOReturn copySyncChannelSequence(void *data, uint32_t length) const {
        if (!data || !_syncChannelSequenceLength || length < _syncChannelSequenceLength)
            return kIOReturnNotReady;
        memcpy(data, _syncChannelSequence, _syncChannelSequenceLength);
        return kIOReturnSuccess;
    }

    void setSyncParams(uint32_t awLen, uint32_t awPeriod, uint32_t extLen, uint32_t syncPeriod);
    bool syncParamsValid() const { return _syncParamsValid; }
    uint32_t availabilityWindowLength() const { return _availabilityWindowLength; }
    uint32_t availabilityWindowPeriod() const { return _availabilityWindowPeriod; }
    uint32_t extensionLength() const { return _extensionLength; }
    uint32_t synchronizationFramePeriod() const { return _synchronizationFramePeriod; }

    IOReturn setSyncFrameTemplate(const void *payload, uint32_t length, bool fromIO80211 = true);
    IOReturn copySyncFrameTemplate(void *payload, uint32_t *length) const;

    void setP2PEnabled(bool enabled) { _p2pEnabled = enabled; }
    bool p2pEnabled() const { return _p2pEnabled; }

    void notePeerTrafficRegistration(bool active);
    uint32_t peerRegistrationCount() const { return _peerRegistrations; }

    bool hasVirtualTransport() const {
        return _awdlInterface != nullptr || _p2pInterface != nullptr;
    }

private:
    static bool isConcreteChannel(uint32_t ch) { return ch > 0 && ch <= 196 && ch != 0xff; }
    void bootstrapNative(); // caller holds workloop gate
    void flushActions();
    void drainActions(uint64_t now);
    bool _nativeSchedule = false;
    uint32_t _appleControlMask = 0;
    uint64_t _lastAppleControlUS = 0;
    uint64_t _vifEnabledUS = 0;
    uint64_t _fallbackDeadlineUS = 0;
    uint64_t _masterSeenUS = 0;
    uint32_t _dataRx = 0, _lastDataRxResult = 0;
    void arm(uint32_t milliseconds);
    void flushData();
    void drainData(uint64_t now, const RTW88AWDL::Window &window);
    static uint64_t nowUS();
    IOTimerEventSource *_timer = nullptr;
    bool _timerAttached = false;
    IOService *_owner = nullptr; // timer owner, non-retained
    uint8_t _localAddress[6] = {};
    uint8_t *_airTemplate = nullptr;
    uint8_t *_txBuffer = nullptr;
    uint32_t _airTemplateLength = 0;
    RTW88AWDL::Action _action;
    RTW88AWDL::Clock _clock;
    uint64_t _nextActionUS = 0, _nextPSFUS = 0, _lastStatsUS = 0;
    uint32_t _lastMIFEAW = 0;
    bool _lastMIFEAWValid = false;
    uint16_t _actionSequence = 0;
    uint32_t _actionTx = 0, _psfTx = 0, _mifTx = 0, _actionRx = 0, _dataTx = 0, _dataDropped = 0, _syncUpdates = 0;
    uint32_t _appleActionQueued = 0, _appleActionTx = 0, _appleActionDropped = 0, _appleActionRestamped = 0;
    uint32_t _bpfTemplateAdoptions = 0;
    uint32_t _blockedWindows = 0, _windowSkips = 0;
    uint32_t _socialSweeps = 0;
    uint64_t _nextSocialSweepUS = 0;
    uint8_t _discoverySocialIndex = 0;
    uint8_t _discoveryChannel = 6;
    uint8_t _observedMasterChannel = 0;
    uint8_t _nextAWChannel = 0;
    uint32_t _masterChannelRaw = 0;
    uint32_t _actionCandidates = 0, _actionParseRejected = 0, _actionClockRejected = 0;
    uint32_t _mifRx = 0, _versionRx = 0, _mifPeersObserved = 0, _versionPeersObserved = 0;
    uint64_t _lastRejectReportUS = 0, _lastActionRxUS = 0;
    IOReturn _lastChannelResult = kIOReturnNotReady;
    struct Peer {
        uint8_t mac[6] = {};
        uint64_t seenUS = 0;
        RTW88AWDL::Sequence sequence;
        RTW88AWDL::Clock clock;
        bool sawMIF = false, versionValid = false, valid = false, announced = false;
        uint64_t lastPresenceUS = 0;
        uint8_t version = 0, deviceClass = 0;
    } _peers[32];
    struct Pending { mbuf_t packet = nullptr; uint64_t queuedUS = 0; } _pending[128];
    uint16_t _queueHead = 0, _queueCount = 0;
    struct PendingAction { mbuf_t packet = nullptr; uint64_t queuedUS = 0; } _pendingActions[32];
    uint8_t _actionQueueHead = 0, _actionQueueCount = 0;
    RTW88IEEE80211 *_backend = nullptr;       // non-retained, controller owns
    IOWorkLoop *_workLoop = nullptr;          // non-retained, controller owns
    IO80211VirtualInterface *_awdlInterface = nullptr;
    IO80211VirtualInterface *_p2pInterface = nullptr;

    uint8_t *_syncTemplate = nullptr;
    uint32_t _syncTemplateLength = 0;
    uint32_t _electionMetric = 0;
    uint32_t _electionId = 0;
    uint32_t _masterChannel = 0;
    uint32_t _secondaryMasterChannel = 0;
    uint32_t _peerRegistrations = 0;
    uint32_t _presenceMode = 0;
    uint32_t _syncState = 0;
    uint64_t _actionFrameTxMode = 0;
    uint8_t _bssid[6] = {};
    uint8_t _minRate = 0;
    uint8_t _deviceCapabilities = 0;
    uint8_t _syncChannelSequence[512] = {};
    uint32_t _syncChannelSequenceLength = 0;
    uint32_t _availabilityWindowLength = 0;
    uint32_t _availabilityWindowPeriod = 0;
    uint32_t _extensionLength = 0;
    uint32_t _synchronizationFramePeriod = 0;
    bool _syncParamsValid = false;
    bool _syncEnabled = true;
    bool _p2pEnabled = false;
};
