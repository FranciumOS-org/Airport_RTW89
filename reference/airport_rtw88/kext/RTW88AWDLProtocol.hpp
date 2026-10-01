/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Wire-format facts: AWDL action header and TLVs, independently implemented.
 * References and supported subset are documented in V22_AWDL.md.
 * This header deliberately has no IOKit dependency so production parsing and
 * time arithmetic can run under ASan/UBSan with malformed radio frames. */
namespace RTW88AWDL {
constexpr uint8_t BSSID[6] = {0x00,0x25,0x00,0xff,0x94,0x73};
constexpr unsigned MaxFrame = 4096;
// Decode an MSDU independently of the 802.11/QoS envelope. A-MSDU carries
// one such envelope per subframe, preceded by DA, SA and a big-endian length.
inline bool dataPayload(const uint8_t *p, size_t n, uint16_t &etherType) {
    static const uint8_t snap[8] = {0xaa,0xaa,3,0,0x17,0xf2,8,0};
    if (!p || n < 16 || memcmp(p, snap, sizeof(snap)) ||
        p[8] != 3 || p[9] != 4) return false;
    etherType = uint16_t(p[14]) << 8 | p[15];
    return true;
}
template <typename Deliver>
bool receiveAggregate(const uint8_t *p, size_t n, const uint8_t *local, Deliver deliver) {
    if (!p || !local || !n) return false;
    // Validate the complete aggregate before delivering any of its packets.
    // A malformed trailing subframe must not produce a partial delivery.
    for (unsigned pass = 0; pass < 2; ++pass) {
        size_t offset = 0;
        while (offset < n) {
            if (n - offset < 14) return false;
            const uint8_t *sub = p + offset;
            const size_t length = size_t(sub[12]) << 8 | sub[13];
            uint16_t type = 0;
            if (length > n - offset - 14 ||
                !dataPayload(sub + 14, length, type)) return false;
            if (pass && ((sub[0] & 1) || !memcmp(sub, local, 6)))
                deliver(sub, sub + 6, type, sub + 30, length - 16);
            offset += 14 + length;
            if (offset == n) break;
            const size_t padding = (4 - ((14 + length) & 3)) & 3;
            if (n - offset < padding + 14) return false;
            offset += padding;
        }
    }
    return true;
}
inline uint16_t le16(const uint8_t *p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
inline uint32_t le32(const uint8_t *p) { return uint32_t(le16(p)) | uint32_t(le16(p+2))<<16; }
inline void put16(uint8_t *p, uint16_t v) { p[0]=uint8_t(v); p[1]=uint8_t(v>>8); }
inline void put32(uint8_t *p, uint32_t v) { for (unsigned i=0;i<4;++i) p[i]=uint8_t(v>>(8*i)); }
inline bool unicast(const uint8_t *p) {
    uint8_t any=0; for (unsigned i=0;i<6;++i) any|=p[i];
    return any && !(p[0]&1);
}
struct Sequence {
    uint8_t channel[16] = {};
    uint8_t count = 0, stride = 0;
};
inline bool parseSequence(const uint8_t *p, size_t n, Sequence &out) {
    if (!p || n<6 || p[0]!=15 || p[2]!=0 || p[3]>15) return false;
    const unsigned width=p[1]==0 ? 1 : (p[1]==1 || p[1]==3 ? 2 : 0);
    if (!width || n<6+16*width) return false;
    // 0xffff means no fill beyond the sequence; support the common full list.
    if (le16(p+4)!=0xffff) return false;
    Sequence value; value.count=16; value.stride=uint8_t(p[3]+1);
    for (unsigned i=0;i<16;++i) {
        uint8_t ch=p[6+i*width+(p[1]==1 ? 1 : 0)];
        if (ch>196) return false; // 0 explicitly means unavailable.
        value.channel[i]=ch;
    }
    out=value;
    return true;
}
struct Action {
    uint16_t actionOffset=0, syncOffset=0;
    uint16_t awPeriod=0, afPeriod=0, countdown=0, awCounter=0, commonLength=0;
    uint8_t subtype=0, presence=0, nextAwChannel=0, masterChannel=0, master[6]={};
    Sequence sequence;
    bool electionValid=false, versionValid=false;
    uint8_t version=0, deviceClass=0;
    uint8_t syncAddress[6]={};
    uint32_t masterCounter=0, masterMetric=0, height=0;
};
inline bool actionHeader(const uint8_t *p, size_t n) {
    return p && n>=16 && p[0]==127 && p[1]==0 && p[2]==0x17 &&
        p[3]==0xf2 && p[4]==8 && p[5]==0x10 && (p[6]==0 || p[6]==3);
}
struct ParseFailure { const char *reason="ok"; size_t offset=0; };
inline bool parseAction(const uint8_t *p, size_t n, Action &out, ParseFailure *failure=nullptr) {
    if (failure) *failure=ParseFailure{};
    auto reject=[&](const char *reason, size_t offset)->bool {
        if (failure) { failure->reason=reason; failure->offset=offset; }
        return false;
    };
    if (!p || n>MaxFrame) return reject("frame-size",0);
    Action a;
    if (!actionHeader(p,n)) {
        if (n<40 || p[0]!=0xd0 || (p[1]&0xc7) ||
            memcmp(p+16,BSSID,6) || !unicast(p+10) || !actionHeader(p+24,n-24)) return reject("action-header",0);
        a.actionOffset=24;
    }
    a.subtype=p[a.actionOffset+6];
    bool haveSync=false, haveSequence=false, haveElection=false, haveVersion=false;
    uint8_t electedMaster[6]={};
    Sequence embedded;
    size_t pos=a.actionOffset+16;
    while (pos<n) {
        if (n-pos<3) return reject("trailing-tlv-header",pos);
        const unsigned type=p[pos], len=le16(p+pos+1);
        pos+=3;
        if (len>n-pos) return reject("truncated-tlv",pos-3);
        const uint8_t *v=p+pos;
        if (type==4) {
            if (haveSync || len<33) return reject("sync-size-or-duplicate",pos);
            haveSync=true; a.syncOffset=uint16_t(pos);
            /* OpenAWDL's synchronization-parameters TLV carries the concrete
             * next-AW and master channel in bytes 0 and 3 respectively. Keep
             * them separate from the channel-sequence fill sentinel: 0xff is
             * not a real channel. */
            a.nextAwChannel=v[0]; a.masterChannel=v[3];
            a.countdown=le16(v+1); a.awPeriod=le16(v+5); a.afPeriod=le16(v+7);
            a.commonLength=le16(v+13); a.presence=v[27]; a.awCounter=le16(v+29);
            memcpy(a.master,v+21,6);
            if (!a.awPeriod || a.awPeriod>1024 || !a.afPeriod || a.afPeriod>4096 ||
                !a.presence || a.presence>16 || (a.presence&(a.presence-1)) ||
                a.countdown>uint32_t(a.awPeriod)*a.presence ||
                a.commonLength>a.awPeriod*a.presence || !unicast(a.master)) return reject("sync-fields",pos);
            if (len>33 && !parseSequence(v+33,len-33,embedded)) return reject("embedded-sequence",pos+33);
        } else if (type==24) {
            if (haveElection || len<40) return reject("election-size-or-duplicate",pos);
            haveElection=true;
            memcpy(electedMaster,v,6); memcpy(a.syncAddress,v+6,6);
            a.masterCounter=le32(v+12); a.height=le32(v+16); a.masterMetric=le32(v+20);
            if (!unicast(electedMaster) || !unicast(a.syncAddress) || a.height>10) return reject("election-fields",pos);
        } else if (type==18) {
            if (haveSequence || !parseSequence(v,len,a.sequence)) return reject("sequence",pos);
            haveSequence=true;
        } else if (type==21) {
            if (haveVersion || len<2) return reject("version-size-or-duplicate",pos);
            haveVersion=true; a.version=v[0]; a.deviceClass=v[1];
        }
        pos+=len;
    }
    if (!haveSync) return reject("missing-sync",n);
    if (haveElection && memcmp(electedMaster,a.master,6)) return reject("master-mismatch",n);
    a.electionValid=haveElection; a.versionValid=haveVersion;
    if (!haveSequence) a.sequence=embedded;
    if (!a.sequence.count || a.sequence.stride!=a.presence) return reject("sequence-presence",n);
    out=a;
    return true;
}

// Driver-owned fallback when IO80211 never submits SYNC_FRAME_TEMPLATE.
// Wire layout references: seemoo-lab/owl src/frame.h and protocol paper,
// https://owlink.org/wiki/ (see V43_AWDL.md). No private Apple payload layout.
constexpr size_t NativeFrameSize = 320;
inline uint8_t opClassForChannel(uint8_t channel) {
    if (!channel) return 0;
    return channel <= 14 ? 0x51 : 0x80;
}
inline size_t buildNativeAction(uint8_t *out, size_t capacity, const uint8_t *local,
                               const Action &a, uint8_t subtype) {
    if (!out || capacity<NativeFrameSize || !local || !unicast(local) ||
        !unicast(a.master) || !unicast(a.syncAddress) || a.sequence.count!=16 ||
        a.sequence.stride!=a.presence || !a.presence || a.presence>16 ||
        (a.presence&(a.presence-1)) || !a.awPeriod || a.awPeriod>1024 ||
        !a.afPeriod || a.afPeriod>4096 || a.commonLength>a.awPeriod*a.presence ||
        a.height>10 || (subtype!=0 && subtype!=3)) return 0;
    for(unsigned i=0;i<16;++i) if(a.sequence.channel[i]>196) return 0;
    memset(out,0,NativeFrameSize);
    out[0]=0xd0; memset(out+4,255,6); memcpy(out+10,local,6); memcpy(out+16,BSSID,6);
    const uint8_t header[]={127,0,0x17,0xf2,8,0x10,subtype,0};
    memcpy(out+24,header,sizeof(header));
    size_t offset=40;
    auto tlv=[&](uint8_t type, unsigned length)->uint8_t* {
        if (offset + 3 + length > capacity) return nullptr;
        out[offset]=type;put16(out+offset+1,uint16_t(length));
        uint8_t *v=out+offset+3;offset+=3+length;return v;
    };
    // OpenAWDL/OWL transmits the 16-entry channel sequence using the
    // operating-class representation.  The social channels used by AWDL are
    // 6/44/149 -> operating classes 0x51/0x80/0x80 respectively.
    auto sequence=[&](uint8_t *v) {
        v[0]=15;v[1]=3;v[2]=0;v[3]=a.presence-1;put16(v+4,0xffff);
        for (unsigned i=0;i<16;++i) {
            v[6+i*2]=a.sequence.channel[i];
            v[7+i*2]=opClassForChannel(a.sequence.channel[i]);
        }
    };
    // 33-byte fixed synchronization payload + 38-byte OPCLASS sequence +
    // two padding bytes, matching the public OWL wire layout.
    uint8_t *v=tlv(4,73); if(!v) return 0;
    v[0]=a.sequence.channel[0];put16(v+1,a.awPeriod*a.presence);
    v[3]=a.sequence.channel[0];put16(v+5,a.awPeriod);put16(v+7,a.afPeriod);
    put16(v+9,0x1800);put16(v+11,a.awPeriod);put16(v+13,a.commonLength);
    v[17]=v[18]=v[19]=v[20]=a.presence-1;memcpy(v+21,a.master,6);v[27]=a.presence;
    sequence(v+33);
    v=tlv(5,21); if(!v) return 0; v[3]=uint8_t(a.height);memcpy(v+5,a.master,6);
    put32(v+11,a.masterMetric);put32(v+15,60);
    // 38-byte sequence plus the three padding bytes used by OWL.
    v=tlv(18,41); if(!v) return 0; sequence(v);
    v=tlv(24,40); if(!v) return 0; memcpy(v,a.master,6);memcpy(v+6,a.syncAddress,6);
    put32(v+12,a.masterCounter);put32(v+16,a.height);put32(v+20,a.masterMetric);put32(v+24,60);
    v=tlv(6,9); if(!v) return 0; // Empty service bitmap; mDNS is carried by the host's IPv6 stack.
    // OWL advertises HT capabilities and ARPA only in MIFs, not PSFs.  Keeping
    // that distinction matters because MIF is the peer-promotion frame.
    if (subtype==3) {
        v=tlv(7,8); if(!v) return 0;
        put16(v,0);put16(v+2,0x11ce);v[4]=0x1b;v[5]=0xff;put16(v+6,0);
        v=tlv(16,22); if(!v) return 0; v[0]=3;v[1]=18;memcpy(v+2,"rtw88-",6);
        const char digits[]="0123456789abcdef";
        for(unsigned i=0;i<6;++i){v[8+2*i]=digits[local[i]>>4];v[9+2*i]=digits[local[i]&15];}
        v[20]=0xc0;v[21]=0x0c; // compressed .local suffix
    }
    v=tlv(12,15); if(!v) return 0; put16(v,0x8f24);v[2]='X';v[3]='0';
    uint16_t social=0;
    for(unsigned i=0;i<16;++i) social |= a.sequence.channel[i]==6 ? 1 : a.sequence.channel[i]==44 ? 2 : a.sequence.channel[i]==149 ? 4 : 0;
    put16(v+5,social);memcpy(v+7,local,6);
    v=tlv(21,2); if(!v) return 0; v[0]=0x34;v[1]=1; // AWDL 3.4 / macOS
    return offset;
}

struct Clock {
    uint64_t epochUS=0;
    uint16_t epochAW=0, periodTU=0;
    uint8_t presence=0;
    bool set(const Action &a, uint64_t now) {
        const uint64_t elapsed=(uint64_t(a.awPeriod)*a.presence-a.countdown)*1024;
        if (now<elapsed) return false;
        epochUS=now-elapsed;
        epochAW=uint16_t(a.awCounter-(a.awCounter%a.presence));
        periodTU=a.awPeriod; presence=a.presence;
        return true;
    }
};
struct Window {
    uint16_t aw=0, remainingTU=0;
    uint8_t slot=0, channel=0;
    uint64_t elapsedUS=0, remainingUS=0;
};
inline Window window(const Clock &clock, const Sequence &seq, uint64_t now) {
    Window w;
    if (!clock.periodTU || !clock.presence || !seq.count || !seq.stride || now<clock.epochUS) return w;
    const uint64_t periodUS=uint64_t(clock.periodTU)*1024;
    const uint64_t eawUS=periodUS*clock.presence;
    const uint64_t elapsed=now-clock.epochUS;
    w.aw=uint16_t(clock.epochAW+elapsed/periodUS);
    w.slot=uint8_t((w.aw/seq.stride)%seq.count);
    w.channel=seq.channel[w.slot];
    w.elapsedUS=elapsed%eawUS; w.remainingUS=eawUS-w.elapsedUS;
    w.remainingTU=uint16_t((w.remainingUS+1023)/1024);
    return w;
}
inline bool insideGuard(const Window &w) {
    return w.channel && w.elapsedUS>=3*1024 && w.remainingUS>3*1024;
}
inline bool multicastWindow(const Window &w) { return w.slot==0 || w.slot==10; }
inline void stamp(uint8_t *frame, const Action &a, const Window &w, uint64_t now, uint16_t sequence) {
    uint8_t *hdr=frame+a.actionOffset;
    put32(hdr+8,uint32_t(now)); put32(hdr+12,uint32_t(now));
    uint8_t *sync=frame+a.syncOffset;
    sync[0]=w.channel;
    put16(sync+1,w.remainingTU); put16(sync+29,w.aw); put16(sync+31,w.aw);
    const uint64_t commonUS=uint64_t(a.commonLength)*1024;
    put16(sync+15,uint16_t(w.elapsedUS<commonUS ? (commonUS-w.elapsedUS)/1024 : 0));
    if (a.actionOffset==24) put16(frame+22,uint16_t((sequence&4095)<<4));
}
} // namespace RTW88AWDL
