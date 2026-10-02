/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The seam between the two kexts of the native Wi-Fi driver.
 *
 * The front (AirPortRTW89Front) is the IO80211Controller macOS sees. It links
 * against IO80211FamilyLegacy, which OpenCore injects, so it has to be
 * injected as well and only changes with a restart. It knows nothing about
 * the chip and decides nothing: every request is passed to the back.
 *
 * The back is the rtw89 driver proper, loaded and unloaded at run time like
 * any kext. It finds the front in the registry by class name (no link
 * dependency) and hands over a table of functions through
 * callPlatformFunction(); the front answers with its own table. Both tables
 * are plain C, so the back needs the IO80211 headers only for the request
 * structures.
 *
 * Calls into the back are made under a lock that disconnecting takes for
 * writing: once RTW89_FRONT_DISCONNECT returns, no call is in progress and
 * none will follow. Calls into the front are valid from connect until
 * disconnect returns.
 */
#ifndef RTW89_FRONT_API_H
#define RTW89_FRONT_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/kernel_types.h>   /* mbuf_t */

#define RTW89_FRONT_API_VERSION 2
#define RTW89_FRONT_CLASS       "AirPortRTW89Front"
/* callPlatformFunction(name, true, back ops, front ops out, 0, 0) */
#define RTW89_FRONT_CONNECT     "RTW89FrontConnect"
#define RTW89_FRONT_DISCONNECT  "RTW89FrontDisconnect"

#ifdef __cplusplus
extern "C" {
#endif

/* What the front does for the back. @ctx is the first argument of each. */
struct rtw89_front_ops {
    uint32_t version;
    void *ctx;
    void *interface;        /* the IO80211Interface, for requests that name it */

    /* interface->postMessage(): tell IO80211 that something happened */
    void (*post_message)(void *ctx, unsigned int msg, void *data, size_t len);
    /* Link state towards the network stack and IO80211. @reason: the 802.11
     * reason code when it goes down. */
    void (*set_link)(void *ctx, bool up, uint64_t bits_per_second, unsigned int reason);
    /* A packet buffer of the controller's, and handing a received Ethernet
     * frame to the stack (takes the mbuf). */
    mbuf_t (*alloc_packet)(void *ctx, unsigned int len);
    void (*input_packet)(void *ctx, mbuf_t m);
    /* The back has room again after output() returned a stall. */
    void (*tx_wake)(void *ctx);
    /* The family's own handling of a request the back does not want. */
    int (*super_ioctl)(void *ctx, void *interface, void *virtual_interface, void *ifnet,
                       unsigned long cmd, void *data);
    int (*super_ioctl_set)(void *ctx, void *interface, void *virtual_interface,
                           void *skywalk_interface, void *data);
    /* Who does the key handshake of the next join: IO80211's own supplicant
     * (true; networks with 802.1X sign-in) or the driver (false). With true,
     * received EAPOL frames are handed to IO80211 and the keys come back as
     * APPLE80211_IOC_CIPHER_KEY requests. */
    void (*set_apple_rsn)(void *ctx, bool on);
};

/* What the back does for the front. */
struct rtw89_back_ops {
    uint32_t version;
    void *ctx;

    void (*get_mac)(void *ctx, uint8_t mac[6]);
    int (*set_mac)(void *ctx, const uint8_t mac[6]);
    /* IO80211Controller::apple80211Request(): one get or set of one item */
    int (*request)(void *ctx, unsigned int type, int number, void *interface, void *data);
    /* The two ioctl entry points above it. *@handled false: the front goes on
     * to the family's handling. */
    int (*ioctl)(void *ctx, void *interface, void *virtual_interface, void *ifnet,
                 unsigned long cmd, void *data, bool *handled);
    int (*ioctl_set)(void *ctx, void *interface, void *virtual_interface,
                     void *skywalk_interface, void *data, bool *handled);
    /* The interface is brought up or taken down by the stack. */
    int (*enable)(void *ctx, bool on);
    /* An Ethernet frame to send. Returns an IOOutputQueue status
     * (kIOReturnOutputSuccess, ...Stall, ...Dropped); the mbuf is the back's
     * unless it stalls. */
    uint32_t (*output)(void *ctx, mbuf_t m);
    /* The system goes to sleep (false) or has woken (true). */
    void (*power)(void *ctx, bool on);
};

#ifdef __cplusplus
}
#endif
#endif
