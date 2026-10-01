/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * RTW88UserClient.hpp — IOUserClient for rtw88ctl IPC
 *
 * Selector numbers must match ctl/main.c RTW88_CMD_* defines.
 */
#pragma once

#include <IOKit/IOUserClient.h>

class RTW88PCIDevice;

/* ------------------------------------------------------------------ */
/*  Selector constants — keep in sync with ctl/main.c                  */
/* ------------------------------------------------------------------ */
enum RTW88UserClientSelector {
    kRTW88Scan        = 0,
    kRTW88Connect     = 1,
    kRTW88Disconnect  = 2,
    kRTW88GetState    = 3,
    kRTW88GetBSSList  = 4,
    kRTW88GetRSSI     = 5,
    kRTW88SetDebug    = 6,
    kRTW88GetLog      = 7,
    kRTW88PowerOn     = 8,
    kRTW88PowerOff    = 9,
    kRTW88NumSelectors
};

/* Structures passed through IOConnectCallStructMethod */
struct RTW88ConnectArgs {
    char ssid[33];
    char password[64];
};

struct RTW88StateResult {
    uint32_t state;
    uint8_t  bssid[6];
    char     ssid[33];
    int32_t  rssi;
    uint32_t channel;
    uint8_t  mac_addr[6];
    uint16_t fw_version;
    uint8_t  fw_sub_version;
    char     chip_name[32];
    uint32_t rx_byte_count;
    uint32_t tx_byte_count;
    uint8_t  scan_offload_supported;
    uint8_t  powered;
};

/* ------------------------------------------------------------------ */
class RTW88UserClient : public IOUserClient {
    OSDeclareDefaultStructors(RTW88UserClient)

public:
    static RTW88UserClient *create(RTW88PCIDevice *dev, task_t owningTask);

    bool     init(OSDictionary *props) override;
    bool     initWithTask(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties) override;
    bool     initWithTask(task_t owningTask, void *securityID, UInt32 type) override;
    bool     start(IOService *provider) override;
    void     stop(IOService *provider) override;
    void     free() override;

    /* IOUserClient */
    IOReturn clientClose() override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                            IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) override;

private:
    /* Dispatch table */
    static IOReturn sScan(RTW88UserClient *target, void *ref,
                          IOExternalMethodArguments *args);
    static IOReturn sConnect(RTW88UserClient *target, void *ref,
                             IOExternalMethodArguments *args);
    static IOReturn sDisconnect(RTW88UserClient *target, void *ref,
                                IOExternalMethodArguments *args);
    static IOReturn sGetState(RTW88UserClient *target, void *ref,
                              IOExternalMethodArguments *args);
    static IOReturn sGetBSSList(RTW88UserClient *target, void *ref,
                                IOExternalMethodArguments *args);
    static IOReturn sGetRSSI(RTW88UserClient *target, void *ref,
                              IOExternalMethodArguments *args);
    static IOReturn sSetDebug(RTW88UserClient *target, void *ref,
                               IOExternalMethodArguments *args);
    static IOReturn sGetLog(RTW88UserClient *target, void *ref,
                             IOExternalMethodArguments *args);
    static IOReturn sPowerOn(RTW88UserClient *target, void *ref,
                             IOExternalMethodArguments *args);
    static IOReturn sPowerOff(RTW88UserClient *target, void *ref,
                              IOExternalMethodArguments *args);

    static const IOExternalMethodDispatch sMethods[kRTW88NumSelectors];

    RTW88PCIDevice *_provider    = nullptr;
    task_t          _owningTask  = nullptr;
};
