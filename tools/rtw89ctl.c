// SPDX-License-Identifier: GPL-2.0
/*
 * rtw89ctl: talk to a loaded AirPort_RTW89 kext.
 *
 *   sudo rtw89ctl up        start the radio (power on, firmware, calibration)
 *   sudo rtw89ctl scan      start the radio if needed, scan, print the networks
 *   sudo rtw89ctl join SSID join a network from the last scan; asks for the
 *                           password (just press Return for an open network)
 *   sudo rtw89ctl leave     leave the network
 *   sudo rtw89ctl down      stop the radio
 *        rtw89ctl status    print what the kext has published
 *   sudo rtw89ctl flush on|off   pass on received frames whose status report
 *                           from the chip is missing after 2 ms (default on)
 *
 * Once a network is joined, traffic flows through the Ethernet-style
 * interface the kext publishes (the "interface" line of status, e.g. en7):
 * macOS configures it with DHCP like any other Ethernet port.
 *
 * Commands go in through IORegistryEntrySetCFProperties (the kext requires an
 * administrator); results come back as registry properties.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <net/if.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static io_service_t find_service(void)
{
    return IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AirPort_RTW89"));
}

/* @ssid, @passphrase: sent along as raw bytes when not NULL (for "join"). */
static kern_return_t send_command_join(io_service_t service, const char *command,
                                       const char *ssid, const char *passphrase)
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
    if (passphrase && passphrase[0]) {
        CFDataRef data = CFDataCreate(NULL, (const UInt8 *)passphrase, (CFIndex)strlen(passphrase));

        CFDictionarySetValue(dict, CFSTR("RTW89Passphrase"), data);
        CFRelease(data);
    }
    kr = IORegistryEntrySetCFProperties(service, dict);

    CFRelease(dict);
    CFRelease(value);
    return kr;
}

static kern_return_t send_command(io_service_t service, const char *command)
{
    return send_command_join(service, command, NULL, NULL);
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
    if (error == -2015 || error == -2002)
        printf("%-10s the AP gave up on the key handshake (802.11 reason %ld): "
               "usually a wrong password\n", "last error", -error - 2000);
    else if (error <= -2000)
        printf("%-10s the AP ended the connection, 802.11 reason %ld\n", "last error", -error - 2000);
    else if (error <= -1000)
        printf("%-10s the AP refused, 802.11 status %ld\n", "last error", -error - 1000);
    else if (error == -110)
        printf("%-10s timed out waiting for the AP\n", "last error");
    else
        printf("%-10s %ld\n", "last error", error);
}

/* The BSD name (en7, ...) of the kext's network interface, once it has one. */
static int get_interface(io_service_t service, char *name, size_t len)
{
    CFTypeRef v = IORegistryEntrySearchCFProperty(service, kIOServicePlane, CFSTR("BSD Name"),
                                                  NULL, kIORegistryIterateRecursively);
    int ok = v && CFGetTypeID(v) == CFStringGetTypeID() &&
             CFStringGetCString(v, name, (CFIndex)len, kCFStringEncodingUTF8);

    if (v)
        CFRelease(v);
    return ok;
}

static void print_interface(io_service_t service)
{
    char name[32];

    if (get_interface(service, name, sizeof(name)))
        printf("%-10s %s\n", "interface", name);
}

/* "0, 5" for the bits set in @tids. */
static const char *tid_list(long tids, char *buf, size_t len)
{
    size_t out = 0;
    int tid;

    buf[0] = 0;
    for (tid = 0; tid < 16 && out + 4 < len; tid++)
        if (tids & (1L << tid))
            out += (size_t)snprintf(buf + out, len - out, "%s%d", out ? ", " : "", tid);
    return out ? buf : "none";
}

static int run(const char *path, const char *a1, const char *a2, const char *a3)
{
    char *argv[] = { (char *)path, (char *)a1, (char *)a2, (char *)a3, NULL };
    int status = -1;
    pid_t pid;

    if (posix_spawn(&pid, path, NULL, NULL, argv, environ))
        return -1;
    waitpid(pid, &status, 0);
    return status;
}

/*
 * macOS only configures interfaces that belong to a network service, and it
 * does not create one for a port it has not seen before. If nothing has
 * brought the interface up, do what a service would: up, and DHCP. Once the
 * port has been added in System Settings this finds it up and leaves it alone.
 */
static void configure_interface(io_service_t service)
{
    struct ifreq ifr;
    char name[32];
    int fd, up = 1;

    if (geteuid() || !get_interface(service, name, sizeof(name)))
        return;

    memset(&ifr, 0, sizeof(ifr));
    strlcpy(ifr.ifr_name, name, sizeof(ifr.ifr_name));
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0) {
        if (!ioctl(fd, SIOCGIFFLAGS, &ifr))
            up = ifr.ifr_flags & IFF_UP;
        close(fd);
    }
    if (up)
        return;

    printf("%-10s bringing %s up with DHCP (no network service uses it)\n", "interface", name);
    run("/sbin/ifconfig", name, "up", NULL);
    run("/usr/sbin/ipconfig", "set", name, "DHCP");
}

static int get_bool(io_service_t service, CFStringRef key);

static void print_link(io_service_t service)
{
    char state[32], bssid[32];

    get_string(service, CFSTR("RTW89 Link State"), state, sizeof(state));
    get_string(service, CFSTR("RTW89 Link BSSID"), bssid, sizeof(bssid));
    if (!state[0])
        return;
    printf("%-10s %s\n", "link", state);
    if (strcmp(state, "down")) {
        static const char *const modes[] = { "802.11a/b/g", "802.11n", "802.11ac" };
        long mode = get_long(service, CFSTR("RTW89 Link Mode"));

        printf("%-10s %s, %ld MHz, AID %ld\n", "network", bssid,
               get_long(service, CFSTR("RTW89 Link Frequency")),
               get_long(service, CFSTR("RTW89 Link AID")));
        printf("%-10s %s, %ld MHz wide (centre %ld MHz), %ld stream(s)\n", "channel",
               mode >= 0 && mode <= 2 ? modes[mode] : "?",
               get_long(service, CFSTR("RTW89 Link Width")),
               get_long(service, CFSTR("RTW89 Link Center")),
               get_long(service, CFSTR("RTW89 Link Streams")));
        printf("%-10s %ld EAPOL frame(s) from the AP\n", "handshake",
               get_long(service, CFSTR("RTW89 Link EAPOL Frames")));
    }
    print_interface(service);
    printf("%-10s sent %ld (dropped %ld), received %ld (dropped %ld: %ld not decrypted, "
           "%ld replayed, %ld duplicates)\n", "frames",
           get_long(service, CFSTR("RTW89 TX Frames")), get_long(service, CFSTR("RTW89 TX Dropped")),
           get_long(service, CFSTR("RTW89 RX Frames")), get_long(service, CFSTR("RTW89 RX Dropped")),
           get_long(service, CFSTR("RTW89 RX Undecrypted")),
           get_long(service, CFSTR("RTW89 RX Replayed")),
           get_long(service, CFSTR("RTW89 RX Duplicates")));
    printf("%-10s %ld frame(s) reached the driver more than 5 ms after the chip received them "
           "(worst %ld ms; %ld although the chip interrupted on arrival); %ld passed on "
           "without a status report (%s)\n", "timing",
           get_long(service, CFSTR("RTW89 RX Late")), get_long(service, CFSTR("RTW89 RX Late Max")),
           get_long(service, CFSTR("RTW89 RX Late With Interrupt")),
           get_long(service, CFSTR("RTW89 RX Status Flushed")),
           get_bool(service, CFSTR("RTW89 RX Status Flush")) ? "flush on" : "flush off");
    if (strcmp(state, "down")) {
        char tx[64], rx[64];

        printf("%-10s sending on TIDs: %s; receiving on TIDs: %s (%ld frame(s) released "
               "after waiting for a missing one)\n", "aggregation",
               tid_list(get_long(service, CFSTR("RTW89 TX Aggregation")), tx, sizeof(tx)),
               tid_list(get_long(service, CFSTR("RTW89 RX Aggregation")), rx, sizeof(rx)),
               get_long(service, CFSTR("RTW89 RX Reorder Timeouts")));
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
        printf("%-18s %4s %5s  %-10s %-10s %s\n", "BSSID", "CH", "RSSI", "MODE", "SECURITY", "SSID");
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
        {
            static const char *const modes[] = { "a/b/g", "n", "ac", "ax" };
            long mode = get_number(bss, CFSTR("mode")), sec = get_number(bss, CFSTR("security"));
            char how[24], security[24];

            snprintf(how, sizeof(how), "%s %ld", mode >= 0 && mode <= 3 ? modes[mode] : "?",
                     get_number(bss, CFSTR("width")));
            /* what this driver can join: open and WPA2-PSK without required PMF */
            snprintf(security, sizeof(security), "%s%s",
                     !sec ? "open" :
                     (sec & 0x02) && (sec & 0x04) ? "WPA2/3" :
                     sec & 0x02 ? "WPA2" :
                     sec & 0x04 ? "WPA3" :
                     sec & 0x08 ? "802.1X" : "WEP/WPA",
                     (sec & 0x10) || (sec && !(sec & 0x02)) ? " (no)" : "");
            /* rssi was stored as a 32-bit two's complement number */
            printf("%-18s %4ld %5d  %-10s %-10s %s\n", mac, get_number(bss, CFSTR("channel")),
                   (int)(signed char)get_number(bss, CFSTR("rssi")), how, security,
                   out ? name : "(hidden)");
        }
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
        /* the counters are only refreshed on request */
        send_command(service, "results");
        print_link(service);
        print_results(service);
        return 0;
    }

    if (!strcmp(command, "flush") && argc >= 3 &&
        (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
        kr = send_command(service, !strcmp(argv[2], "on") ? "flush-on" : "flush-off");
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "flush failed: 0x%x%s\n", kr,
                    kr == kIOReturnNotPrivileged ? " (run with sudo)" : "");
            return 1;
        }
        printf("flush %s\n", argv[2]);
        return 0;
    }

    if ((strcmp(command, "up") && strcmp(command, "down") && strcmp(command, "scan") &&
         strcmp(command, "join") && strcmp(command, "leave")) ||
        (!strcmp(command, "join") && argc < 3)) {
        fprintf(stderr, "usage: rtw89ctl up|down|scan|join SSID|leave|status|flush on|off\n");
        return 2;
    }

    if (!strcmp(command, "join")) {
        /* Typed at the prompt, not on the command line, so the password does
         * not end up in the shell history or the process list. */
        char *pass = getpass("Wi-Fi password (Return for an open network): ");

        kr = send_command_join(service, command, argv[2], pass);
        memset(pass, 0, strlen(pass));
    } else {
        kr = send_command(service, command);
    }
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "%s failed: 0x%x%s\n", command, kr,
                kr == kIOReturnNotPrivileged ? " (run with sudo)" : " (see the kernel log)");
        if (!strcmp(command, "join"))
            fprintf(stderr, "is the network in the last scan (rtw89ctl scan), and is the "
                            "password 8 to 63 characters?\n");
        return 1;
    }

    if (!strcmp(command, "join")) {
        char state[32] = "";

        /* Authentication, association and the key handshake take a second or
         * two; a wrong password shows as the AP dropping us after a few more. */
        for (i = 0; i < 48; i++) {
            usleep(250 * 1000);
            get_string(service, CFSTR("RTW89 Link State"), state, sizeof(state));
            if (!strcmp(state, "connected") || (i > 4 && !strcmp(state, "down")))
                break;
        }
        print_link(service);
        if (!strcmp(state, "connected"))
            configure_interface(service);
        return strcmp(state, "connected");
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
