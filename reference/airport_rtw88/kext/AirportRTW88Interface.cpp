/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#include "AirportRTW88Interface.hpp"
#include <sys/kpi_mbuf.h>

#define super IO80211Interface

OSDefineMetaClassAndStructors(AirportRTW88Interface, IO80211Interface)

bool AirportRTW88Interface::init(IONetworkController *controller)
{
    if (!super::init(controller))
        return false;
    return true;
}

UInt32 AirportRTW88Interface::inputPacket(mbuf_t packet, UInt32 length,
                                          IOOptionBits options, void *param)
{
    if (!packet)
        return 0;

    size_t len = mbuf_len(packet);

    /*
     * Match AirportItlwm's EAPOL submission path.
     *
     * Apple RSN needs EAPOL (EtherType 0x888E) delivered through
     * IO80211Interface with the real mbuf length.  Normal Ethernet
     * traffic can continue through the Ethernet superclass path.
     */
    if (len >= 14) {
        const uint8_t *eh = (const uint8_t *)mbuf_data(packet);
        uint16_t ethertype =
            ((uint16_t)eh[12] << 8) |
             (uint16_t)eh[13];

        if (ethertype == 0x888E) {
            IOLog("AirportRTW88: forwarding EAPOL to IO80211 len=%zu\n", len);
            return IO80211Interface::inputPacket(
                packet, (UInt32)mbuf_pkthdr_len(packet), 0, param);
        }
    }

    return IOEthernetInterface::inputPacket(
        packet, length, options, param);
}
