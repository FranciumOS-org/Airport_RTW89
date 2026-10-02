/*
 * Build: xcrun clang -g -framework CoreFoundation tools/assocprobe.c -o build/out/assocprobe
 * Run:   sudo lldb -b -s tools/assocprobe.lldb -- build/out/assocprobe en4 "SSID"
 *
 * Calls macOS's own join function, Apple80211Associate2 (IO80211 private
 * framework), the way airportd does, so that a debugger can see which of its
 * checks turns a join down before anything reaches the driver. Scans, picks
 * the scan entry with that name and uses a dummy password: the checks that
 * matter run before any key exchange, and under lldb a password prompt cannot
 * take the terminal anyway (SIGTTOU). It leaves the current network first, as
 * airportd would; the join itself then fails on the wrong password.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: assocprobe IFNAME SSID\n");
        return 2;
    }
    void *h = dlopen("/System/Library/PrivateFrameworks/IO80211.framework/IO80211", RTLD_NOW);
    int (*open_)(void **) = h ? dlsym(h, "Apple80211Open") : NULL;
    int (*bind_)(void *, CFStringRef) = h ? dlsym(h, "Apple80211BindToInterface") : NULL;
    int (*scan)(void *, CFArrayRef *, CFDictionaryRef) = h ? dlsym(h, "Apple80211Scan") : NULL;
    int (*assoc2)(void *, CFDictionaryRef, CFStringRef, CFDictionaryRef) =
        h ? dlsym(h, "Apple80211Associate2") : NULL;
    if (!open_ || !bind_ || !scan || !assoc2) {
        fprintf(stderr, "IO80211 symbols missing\n");
        return 1;
    }
    void *ref = NULL;
    CFStringRef ifn = CFStringCreateWithCString(NULL, argv[1], kCFStringEncodingUTF8);
    CFStringRef want = CFStringCreateWithCString(NULL, argv[2], kCFStringEncodingUTF8);
    printf("open %d, bind %d\n", open_(&ref), bind_(ref, ifn));

    CFArrayRef list = NULL;
    CFDictionaryRef params = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                &kCFTypeDictionaryValueCallBacks);
    int e = scan(ref, &list, params);
    printf("scan %d: %ld network(s)\n", e, list ? (long)CFArrayGetCount(list) : -1L);
    CFDictionaryRef net = NULL;
    for (CFIndex i = 0; list && i < CFArrayGetCount(list); i++) {
        CFDictionaryRef d = CFArrayGetValueAtIndex(list, i);
        CFStringRef s = CFDictionaryGetValue(d, CFSTR("SSID_STR"));
        if (s && CFEqual(s, want)) {
            net = d;
            break;
        }
    }
    if (!net) {
        fprintf(stderr, "no scan entry named %s\n", argv[2]);
        return 1;
    }
    CFShow(CFDictionaryGetValue(net, CFSTR("RSN_IE")));

    CFStringRef pass = CFSTR("not-the-password");
    e = assoc2(ref, net, pass, NULL);
    printf("Apple80211Associate2: %d\n", e);
    return 0;
}
