// SPDX-License-Identifier: GPL-2.0
/*
 * rtw89ctl: talk to a loaded AirPort_RTW89 kext.
 *
 *   sudo rtw89ctl up        start the radio (power on, firmware, calibration)
 *   sudo rtw89ctl scan      start the radio if needed, scan, print the networks
 *   sudo rtw89ctl join SSID associate with a network from the last scan
 *   sudo rtw89ctl leave     leave the network
 *   sudo rtw89ctl down      stop the radio
 *        rtw89ctl status    print what the kext has published
 *
 * Commands go in through IORegistryEntrySetCFProperties (the kext requires an
 * administrator); results come back as registry properties.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static io_service_t find_service(void)
{
    return IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AirPort_RTW89"));
}

/* @ssid: sent along as raw bytes when not NULL (for "join"). */
static kern_return_t send_command_ssid(io_service_t service, const char *command, const char *ssid)
{
    CFStringRef value = CFStringCreateWithCString(NULL, command, kCFStringEncodingUTF8);
    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(NULL, 2,
                                                            &kCFTypeDictionaryKeyCallBacks,
                                                            &kCFTypeDictionaryValueCallBacks);
    kern_return_t kr;

    CFDictionarySetValue(dict, CFSTR("RTW89Command"), value);
    if (ssid) {
        CFDataRef data = CFDataCreate(NULL, (const UInt8 *)ssid, (CFIndex)strlen(ssid));

        CFDictionarySetValue(dict, CFSTR("RTW89SSID"), data);
        CFRelease(data);
    }
    kr = IORegistryEntrySetCFProperties(service, dict);

    CFRelease(dict);
    CFRelease(value);
    return kr;
}

static kern_return_t send_command(io_service_t service, const char *command)
{
    return send_command_ssid(service, command, NULL);
}

static long get_long(io_service_t service, CFStringRef key)
{
    CFTypeRef v = IORegistryEntryCreateCFProperty(service, key, NULL, 0);
    int value = 0;

    if (v && CFGetTypeID(v) == CFNumberGetTypeID())
        CFNumberGetValue(v, kCFNumberSInt32Type, &value);
    if (v)
        CFRelease(v);
    return value;
}

static int get_string(io_service_t service, CFStringRef key, char *buf, size_t len)
{
    CFTypeRef v = IORegistryEntryCreateCFProperty(service, key, NULL, 0);
    int ok = v && CFGetTypeID(v) == CFStringGetTypeID() &&
             CFStringGetCString(v, buf, (CFIndex)len, kCFStringEncodingUTF8);

    if (v)
        CFRelease(v);
    if (!ok)
        buf[0] = 0;
    return ok;
}

static void print_error(long error)
{
    if (!error)
        return;
    if (error <= -2000)
        printf("%-10s the AP ended the connection, 802.11 reason %ld\n", "last error", -error - 2000);
    else if (error <= -1000)
        printf("%-10s the AP refused, 802.11 status %ld\n", "last error", -error - 1000);
    else
        printf("%-10s %ld\n", "last error", error);
}

static void print_link(io_service_t service)
{
    char state[32], bssid[32];

    get_string(service, CFSTR("RTW89 Link State"), state, sizeof(state));
    get_string(service, CFSTR("RTW89 Link BSSID"), bssid, sizeof(bssid));
    if (!state[0])
        return;
    printf("%-10s %s\n", "link", state);
    if (strcmp(state, "down")) {
        printf("%-10s %s, %ld MHz, AID %ld\n", "network", bssid,
               get_long(service, CFSTR("RTW89 Link Frequency")),
               get_long(service, CFSTR("RTW89 Link AID")));
        printf("%-10s %ld EAPOL frame(s) from the AP\n", "handshake",
               get_long(service, CFSTR("RTW89 Link EAPOL Frames")));
    }
    print_error(get_long(service, CFSTR("RTW89 Link Error")));
}

static int get_bool(io_service_t service, CFStringRef key)
{
    CFTypeRef v = IORegistryEntryCreateCFProperty(service, key, NULL, 0);
    int result = v && CFGetTypeID(v) == CFBooleanGetTypeID() && CFBooleanGetValue(v);

    if (v)
        CFRelease(v);
    return result;
}

static void print_string(io_service_t service, CFStringRef key, const char *label)
{
    CFTypeRef v = IORegistryEntryCreateCFProperty(service, key, NULL, 0);
    char buf[128];

    if (v && CFGetTypeID(v) == CFStringGetTypeID() &&
        CFStringGetCString(v, buf, sizeof(buf), kCFStringEncodingUTF8))
        printf("%-10s %s\n", label, buf);
    if (v)
        CFRelease(v);
}

static long get_number(CFDictionaryRef dict, CFStringRef key)
{
    CFNumberRef n = CFDictionaryGetValue(dict, key);
    int v = 0;

    if (n && CFGetTypeID(n) == CFNumberGetTypeID())
        CFNumberGetValue(n, kCFNumberSInt32Type, &v);
    return v;
}

static void print_results(io_service_t service)
{
    CFArrayRef list = IORegistryEntryCreateCFProperty(service, CFSTR("RTW89 Scan Results"), NULL, 0);
    CFIndex i, n;

    if (!list || CFGetTypeID(list) != CFArrayGetTypeID()) {
        printf("no scan results\n");
        if (list)
            CFRelease(list);
        return;
    }

    n = CFArrayGetCount(list);
    printf("%ld network(s)\n", (long)n);
    if (n)
        printf("%-18s %4s %5s %5s  %s\n", "BSSID", "CH", "RSSI", "SEEN", "SSID");
    for (i = 0; i < n; i++) {
        CFDictionaryRef bss = CFArrayGetValueAtIndex(list, i);
        CFStringRef bssid = CFDictionaryGetValue(bss, CFSTR("bssid"));
        CFDataRef ssid = CFDictionaryGetValue(bss, CFSTR("ssid"));
        char mac[32] = "?";
        char name[4 * 32 + 1];
        size_t out = 0;

        if (bssid)
            CFStringGetCString(bssid, mac, sizeof(mac), kCFStringEncodingUTF8);
        if (ssid && CFGetTypeID(ssid) == CFDataGetTypeID()) {
            const unsigned char *p = CFDataGetBytePtr(ssid);
            CFIndex len = CFDataGetLength(ssid), j;

            /* SSIDs are arbitrary bytes: show printable ASCII and UTF-8 as is,
             * escape control characters. */
            for (j = 0; j < len && out + 5 < sizeof(name); j++) {
                if (p[j] < 0x20 || p[j] == 0x7f)
                    out += snprintf(name + out, sizeof(name) - out, "\\x%02x", p[j]);
                else
                    name[out++] = (char)p[j];
            }
        }
        name[out] = 0;
        /* rssi was stored as a 32-bit two's complement number */
        printf("%-18s %4ld %5d %5ld  %s\n", mac, get_number(bss, CFSTR("channel")),
               (int)(signed char)get_number(bss, CFSTR("rssi")), get_number(bss, CFSTR("seen")),
               out ? name : "(hidden)");
    }
    CFRelease(list);
}

int main(int argc, char **argv)
{
    const char *command = argc > 1 ? argv[1] : "status";
    io_service_t service = find_service();
    kern_return_t kr;
    int i;

    if (!service) {
        fprintf(stderr, "AirPort_RTW89 is not loaded\n");
        return 1;
    }

    if (!strcmp(command, "status")) {
        print_string(service, CFSTR("RTW89 MAC Address"), "MAC");
        print_string(service, CFSTR("RTW89 Firmware Version"), "firmware");
        printf("%-10s %s\n", "radio", get_bool(service, CFSTR("RTW89 Radio Up")) ? "up" : "down");
        print_link(service);
        print_results(service);
        return 0;
    }

    if ((strcmp(command, "up") && strcmp(command, "down") && strcmp(command, "scan") &&
         strcmp(command, "join") && strcmp(command, "leave")) ||
        (!strcmp(command, "join") && argc < 3)) {
        fprintf(stderr, "usage: rtw89ctl up|down|scan|join SSID|leave|status\n");
        return 2;
    }

    kr = send_command_ssid(service, command, !strcmp(command, "join") ? argv[2] : NULL);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "%s failed: 0x%x%s\n", command, kr,
                kr == kIOReturnNotPrivileged ? " (run with sudo)" : " (see the kernel log)");
        if (!strcmp(command, "join"))
            fprintf(stderr, "is the network in the last scan? run: rtw89ctl scan\n");
        return 1;
    }

    if (!strcmp(command, "join")) {
        char state[32] = "";

        /* Authentication and association take well under a second each. */
        for (i = 0; i < 20; i++) {
            usleep(250 * 1000);
            send_command(service, "results");
            get_string(service, CFSTR("RTW89 Link State"), state, sizeof(state));
            if (strcmp(state, "joining"))
                break;
        }
        /* give the AP a moment to start the key handshake */
        if (!strcmp(state, "associated")) {
            sleep(1);
            send_command(service, "results");
        }
        print_link(service);
        return strcmp(state, "associated") && strcmp(state, "connected");
    }
    if (!strcmp(command, "leave")) {
        printf("left the network\n");
        return 0;
    }

    if (!strcmp(command, "scan")) {
        /* A full scan takes a few seconds; poll until the kext says it is over. */
        for (i = 0; i < 60; i++) {
            usleep(500 * 1000);
            send_command(service, "results");
            if (!get_bool(service, CFSTR("RTW89 Scanning")))
                break;
        }
        if (i == 60)
            printf("scan still running after 30 s\n");
        print_results(service);
    } else {
        printf("radio %s\n", command);
    }
    return 0;
}
