/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_IF_ETHER_H
#define _RTW88_COMPAT_IF_ETHER_H

#define ETH_ALEN    6
#define ETH_HLEN    14
#define ETH_FCS_LEN 4
#define ETH_FRAME_LEN 1514
#define ETH_DATA_LEN  1500
#define ETH_MIN_MTU   68
#define ETH_MAX_MTU   0xFFFFU

#define ETH_P_IP    0x0800
#define ETH_P_ARP   0x0806
#define ETH_P_IPV6  0x86DD
#define ETH_P_PAE   0x888E  /* EAPOL — used for 802.1X/WPA handshake */
#define ETH_P_PREAUTH 0x88C7
#define ETH_P_TDLS  0x890D
#define ETH_P_8021Q 0x8100

#endif /* _RTW88_COMPAT_IF_ETHER_H */
