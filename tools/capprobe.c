/* Build: xcrun clang -framework CoreFoundation tools/capprobe.c -o build/out/capprobe
 * Run:   build/out/capprobe en4   (no sudo)
 *
 * Ask the Wi-Fi interface for its card capabilities exactly as macOS's own
 * Apple80211BindToInterfaceWithService does (request 12, 19 bytes) and print
 * what comes back. Read-only. */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct req { char name[16]; uint32_t type; uint32_t val; uint32_t len; uint32_t pad; void *data; };

int main(int argc, char **argv)
{
    const char *ifn = argc > 1 ? argv[1] : "en4";
    void *h = dlopen("/System/Library/PrivateFrameworks/IO80211.framework/IO80211", RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    int (*open_)(void **) = dlsym(h, "Apple80211Open");
    int (*bind_)(void *, CFStringRef) = dlsym(h, "Apple80211BindToInterface");
    int (*rawget)(void *, struct req *) = dlsym(h, "Apple80211RawGet");
    if (!open_ || !bind_ || !rawget) { printf("missing symbol\n"); return 1; }
    void *ref = NULL;
    printf("open %d\n", open_(&ref));
    CFStringRef s = CFStringCreateWithCString(NULL, ifn, kCFStringEncodingUTF8);
    printf("bind %d\n", bind_(ref, s));
    uint8_t buf[64];
    for (uint32_t len = 12; len <= 19; len += 7) {
        struct req r; memset(&r, 0, sizeof(r)); memset(buf, 0xee, sizeof(buf));
        strlcpy(r.name, ifn, sizeof(r.name));
        r.type = 12; r.len = len; r.data = buf;
        int e = rawget(ref, &r);
        printf("len %u -> %d:", len, e);
        for (uint32_t i = 0; i < len + 4; i++) printf(" %02x", buf[i]);
        printf("\n");
    }
    /* the handle's own copy, as the join code reads it */
    uint8_t *p = (uint8_t *)ref;
    printf("handle+0x54..0x6b:");
    for (int i = 0x54; i < 0x6c; i++) printf(" %02x", p[i]);
    printf("\n");
    return 0;
}
