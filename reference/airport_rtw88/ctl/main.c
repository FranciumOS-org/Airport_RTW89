// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
// rtw88ctl — userspace control binary for rtw88 macOS kext

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <unistd.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>

/* ------------------------------------------------------------------ */
/*  Selector numbers — must match RTW88UserClient.hpp                  */
/* ------------------------------------------------------------------ */
enum {
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
};

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

static const char *state_name(uint32_t s)
{
    switch (s) {
    case 0: return "idle";
    case 1: return "scanning";
    case 2: return "authenticating";
    case 3: return "associating";
    case 4: return "handshaking";
    case 5: return "connected";
    case 6: return "disconnecting";
    default: return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/*  IOKit connection helpers                                            */
/* ------------------------------------------------------------------ */
static io_connect_t open_kext(void)
{
    /* IOServiceMatching finds the instantiated C++ IOService object */
    CFMutableDictionaryRef matching = IOServiceMatching("RTW88PCIDevice");
    if (!matching) {
        fprintf(stderr, "rtw88ctl: failed to create matching dict\n");
        return MACH_PORT_NULL;
    }

    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                       matching);
    if (!service) {
        fprintf(stderr, "rtw88ctl: no rtw88 device found "
                "(is rtw88.kext loaded?)\n");
        return MACH_PORT_NULL;
    }

    io_connect_t conn = MACH_PORT_NULL;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
    IOObjectRelease(service);

    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "rtw88ctl: IOServiceOpen failed: %s\n",
                mach_error_string(kr));
        return MACH_PORT_NULL;
    }
    return conn;
}

/* ------------------------------------------------------------------ */
/*  Commands                                                            */
/* ------------------------------------------------------------------ */

static int cmd_list(io_connect_t conn); /* forward */

static int cmd_scan(io_connect_t conn, int wait_secs)
{
    kern_return_t kr = IOConnectCallScalarMethod(conn, kRTW88Scan,
                                                  NULL, 0, NULL, NULL);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "rtw88ctl: scan failed: %s\n", mach_error_string(kr));
        return 1;
    }

    printf("Scanning");
    fflush(stdout);

    /* Poll state until scan is done (kext returns to idle) or timeout.
     * The kernel scan is async; we wait here so the user sees the
     * results immediately after the command returns. */
    int done = 0;
    for (int i = 0; i < wait_secs; i++) {
        sleep(1);
        putchar('.');
        fflush(stdout);

        struct RTW88StateResult result = {};
        size_t sz = sizeof(result);
        if (IOConnectCallStructMethod(conn, kRTW88GetState,
                                      NULL, 0, &result, &sz) == KERN_SUCCESS) {
            /* state==1 is SCANNING; anything else (idle) means done */
            if (result.state != 1) { done = 1; break; }
        }
    }
    putchar('\n');
    if (!done)
        printf("Scan still running after %d s — showing partial results.\n",
               wait_secs);

    return cmd_list(conn);
}

static int cmd_list(io_connect_t conn)
{
    /* Must stay ≤ 4095 bytes — IOKit MIG inband limit is 4096.
     * Larger buffers switch to the OOL descriptor path where
     * args->structureOutput is NULL, causing a silent empty result. */
    uint8_t buf[4095] = {};
    size_t  len = sizeof(buf);

    kern_return_t kr = IOConnectCallStructMethod(conn, kRTW88GetBSSList,
                                                  NULL, 0, buf, &len);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "rtw88ctl: get BSS list failed: %s\n",
                mach_error_string(kr));
        return 1;
    }

    /* Debug: show raw returned length and first 32 bytes */
    fprintf(stderr, "[dbg] IOKit returned len=%zu, first bytes:", len);
    for (size_t i = 0; i < 32 && i < len; i++) fprintf(stderr, " %02x", buf[i]);
    fprintf(stderr, "\n");

    printf("%-33s %-18s %5s %7s %s\n",
           "SSID", "BSSID", "RSSI", "Channel", "Security");
    printf("%s\n",
           "-----------------------------------------------------------------------");

    const uint8_t *p   = buf;
    
    uint32_t explicit_len = 0;
    if (len >= 4) {
        memcpy(&explicit_len, p, 4);
        p += 4;
        /* Ensure we don't read past what was actually copied back */
        if (explicit_len > len) explicit_len = (uint32_t)len;
    }

    const uint8_t *end = buf + explicit_len;
    while (p + 1 <= end) {
        uint8_t ssid_len = *p++;
        if (p + ssid_len + 6 + 2 + 1 + 4 > end) break;

        char ssid[33] = {};
        memcpy(ssid, p, ssid_len);
        p += ssid_len;

        uint8_t bssid[6];
        memcpy(bssid, p, 6); p += 6;

        int16_t rssi = (int16_t)(((uint16_t)p[0] << 8) | p[1]); p += 2;
        uint8_t ch   = *p++;
        uint32_t cipher; memcpy(&cipher, p, 4); p += 4;

        const char *sec = (cipher == 0x000FAC04) ? "WPA2" :
                          (cipher == 0x000FAC02) ? "TKIP" :
                          (cipher == 0) ? "Open" : "Unknown";

        printf("%-33s %02x:%02x:%02x:%02x:%02x:%02x %4d dBm %4d  %s\n",
               ssid,
               bssid[0], bssid[1], bssid[2],
               bssid[3], bssid[4], bssid[5],
               rssi, ch, sec);
    }
    return 0;
}

static int cmd_connect(io_connect_t conn, const char *ssid, const char *pass)
{
    struct RTW88ConnectArgs args = {};
    strlcpy(args.ssid, ssid, sizeof(args.ssid));
    if (pass) strlcpy(args.password, pass, sizeof(args.password));

    kern_return_t kr = IOConnectCallStructMethod(conn, kRTW88Connect,
                                                  &args, sizeof(args),
                                                  NULL, NULL);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "rtw88ctl: connect failed: %s\n",
                mach_error_string(kr));
        return 1;
    }
    printf("Connecting to '%s'...\n", ssid);

    /* Poll state until connected or 30s timeout */
    for (int i = 0; i < 30; i++) {
        sleep(1);
        struct RTW88StateResult result = {};
        size_t sz = sizeof(result);
        kr = IOConnectCallStructMethod(conn, kRTW88GetState,
                                       NULL, 0, &result, &sz);
        if (kr == KERN_SUCCESS) {
            printf("\r[%2ds] state: %-16s", i + 1, state_name(result.state));
            fflush(stdout);
            if (result.state == 5) { /* connected */
                printf("\nConnected! RSSI: %d dBm\n", result.rssi);
                return 0;
            }
            if (result.state == 0 && i > 3) {
                printf("\nConnection failed\n");
                return 1;
            }
        }
    }
    printf("\nConnection timed out\n");
    return 1;
}

static int cmd_disconnect(io_connect_t conn)
{
    kern_return_t kr = IOConnectCallScalarMethod(conn, kRTW88Disconnect,
                                                  NULL, 0, NULL, NULL);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "rtw88ctl: disconnect failed: %s\n",
                mach_error_string(kr));
        return 1;
    }
    printf("Disconnected\n");
    return 0;
}

static int cmd_status(io_connect_t conn)
{
    struct RTW88StateResult result = {};
    size_t sz = sizeof(result);

    kern_return_t kr = IOConnectCallStructMethod(conn, kRTW88GetState,
                                                  NULL, 0, &result, &sz);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "rtw88ctl: get state failed: %s\n",
                mach_error_string(kr));
        return 1;
    }
    printf("State:      %s\n", state_name(result.state));
    printf("Card:       %s\n", result.chip_name);
    printf("MAC:        %02x:%02x:%02x:%02x:%02x:%02x\n",
           result.mac_addr[0], result.mac_addr[1], result.mac_addr[2],
           result.mac_addr[3], result.mac_addr[4], result.mac_addr[5]);
    printf("Firmware:   v%u.%u\n", result.fw_version, result.fw_sub_version);
    printf("Scan offld: %s\n", result.scan_offload_supported ? "yes" : "no");
    printf("Power:      %s\n", result.powered ? "on" : "off");
    
    if (result.state == 5) {
        printf("SSID:       %s\n", result.ssid[0] ? result.ssid : "(unknown)");
        printf("BSSID:      %02x:%02x:%02x:%02x:%02x:%02x\n",
               result.bssid[0], result.bssid[1], result.bssid[2],
               result.bssid[3], result.bssid[4], result.bssid[5]);
        printf("RSSI:       %d dBm\n", result.rssi);
        printf("Channel:    %u\n", result.channel);
        printf("TX Bytes:   %u\n", result.tx_byte_count);
        printf("RX Bytes:   %u\n", result.rx_byte_count);
    }
    return 0;
}

static int cmd_log(io_connect_t conn)
{
    char buf[4096] = {};
    size_t sz = sizeof(buf);
    kern_return_t kr = IOConnectCallStructMethod(conn, kRTW88GetLog,
                                                  NULL, 0, buf, &sz);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "rtw88ctl: get log failed: %s\n",
                mach_error_string(kr));
        return 1;
    }
    fwrite(buf, 1, sz, stdout);
    return 0;
}

static int cmd_debug(io_connect_t conn, int level)
{
    uint64_t in = (uint64_t)level;
    kern_return_t kr = IOConnectCallScalarMethod(conn, kRTW88SetDebug,
                                                  &in, 1, NULL, NULL);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "rtw88ctl: set debug failed: %s\n",
                mach_error_string(kr));
        return 1;
    }
    printf("Debug level set to %d\n", level);
    return 0;
}

static int cmd_power(io_connect_t conn, int on)
{
    kern_return_t kr = IOConnectCallStructMethod(conn,
                                                  on ? kRTW88PowerOn : kRTW88PowerOff,
                                                  NULL, 0, NULL, NULL);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "rtw88ctl: power %s failed: %s\n",
                on ? "on" : "off", mach_error_string(kr));
        return 1;
    }
    printf("Power %s\n", on ? "on" : "off");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Usage                                                               */
/* ------------------------------------------------------------------ */

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s <command> [options]\n"
        "\n"
        "Commands:\n"
        "  scan [-w <secs>]         Scan and print results when done (default 10s)\n"
        "  list                     Show last scan results without re-scanning\n"
        "  connect <ssid> [pass]    Connect to a network\n"
        "  disconnect               Disconnect\n"
        "  power on|off             Toggle IEEE80211 radio power\n"
        "  status                   Show current connection status\n"
        "  log                      Dump driver log buffer\n"
        "  debug <level>            Set debug level (0=err 1=warn 2=info 3=dbg)\n"
        "\n"
        "Examples:\n"
        "  %s scan -w 10            Scan for 10 seconds\n"
        "  %s list                  Show found networks\n"
        "  %s connect \"MyWiFi\" \"password123\"\n"
        "  %s status\n"
        "\n"
        "Notes:\n"
        "  - rtw88.kext must be loaded (OpenCore injection or kextload)\n"
        "  - Works in BaseSystem (Recovery) environment\n"
        "  - Run with sudo if IOServiceOpen fails\n",
        argv0, argv0, argv0, argv0, argv0);
}

/* ------------------------------------------------------------------ */
/*  main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    io_connect_t conn = open_kext();
    if (conn == MACH_PORT_NULL) return 1;

    int ret = 0;
    const char *cmd = argv[1];

    if (strcmp(cmd, "scan") == 0) {
        int wait = 10;
        int opt;
        while ((opt = getopt(argc - 1, argv + 1, "w:")) != -1) {
            if (opt == 'w') wait = atoi(optarg);
        }
        ret = cmd_scan(conn, wait);

    } else if (strcmp(cmd, "list") == 0) {
        ret = cmd_list(conn);

    } else if (strcmp(cmd, "connect") == 0) {
        if (argc < 3) {
            fprintf(stderr, "rtw88ctl: connect requires SSID\n");
            ret = 1;
        } else {
            ret = cmd_connect(conn, argv[2], argc >= 4 ? argv[3] : NULL);
        }

    } else if (strcmp(cmd, "disconnect") == 0) {
        ret = cmd_disconnect(conn);

    } else if (strcmp(cmd, "power") == 0) {
        if (argc < 3 || (strcmp(argv[2], "on") != 0 && strcmp(argv[2], "off") != 0)) {
            fprintf(stderr, "rtw88ctl: power requires on or off\n");
            ret = 1;
        } else {
            ret = cmd_power(conn, strcmp(argv[2], "on") == 0);
        }

    } else if (strcmp(cmd, "status") == 0) {
        ret = cmd_status(conn);

    } else if (strcmp(cmd, "log") == 0) {
        ret = cmd_log(conn);

    } else if (strcmp(cmd, "debug") == 0) {
        if (argc < 3) { fprintf(stderr, "debug requires level\n"); ret = 1; }
        else ret = cmd_debug(conn, atoi(argv[2]));

    } else {
        fprintf(stderr, "rtw88ctl: unknown command '%s'\n", cmd);
        usage(argv[0]);
        ret = 1;
    }

    IOServiceClose(conn);
    return ret;
}
