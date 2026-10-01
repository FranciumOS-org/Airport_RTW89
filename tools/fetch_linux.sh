#!/bin/sh
# Download the unmodified Linux files this port builds against, at the commit
# pinned in third_party/rtw89/UPSTREAM.md.
#
#   third_party/linux-include/    on the include path (see Makefile)
#   third_party/linux-reference/  not compiled; source that compat code is taken
#                                 from (tools/extract_bitrate.sh)
#
# Goes through the GitHub contents API because raw.githubusercontent.com is not
# reachable from every network. Unauthenticated use is limited to 60 requests
# an hour, which one run stays well under.
#
# Usage: tools/fetch_linux.sh [commit]
set -eu
sha=${1:-551c722f40809618230001baccf219193e22fc5a}
api=https://api.github.com/repos/torvalds/linux/contents

fetch() { # <path in linux tree> <destination>
    mkdir -p "$(dirname "$2")"
    curl -fsS -m 120 -H 'Accept: application/vnd.github.raw' -o "$2" "$api/$1?ref=$sha"
    echo "  $2"
}

inc=third_party/linux-include
for f in ieee80211.h ieee80211-ht.h ieee80211-vht.h ieee80211-he.h ieee80211-eht.h \
         ieee80211-uhr.h ieee80211-mesh.h ieee80211-s1g.h ieee80211-p2p.h \
         ieee80211-nan.h pci_ids.h; do
    fetch include/linux/$f $inc/linux/$f
done
for f in nl80211.h pci_regs.h; do
    fetch include/uapi/linux/$f $inc/uapi/linux/$f
done
for f in cfg80211.h mac80211.h regulatory.h ieee80211_radiotap.h; do
    fetch include/net/$f $inc/net/$f
done

fetch net/wireless/util.c third_party/linux-reference/net/wireless/util.c
# mac80211 internals the stand-in in src/compat_rtw89/rtw89_mac80211.c follows
for f in iface.c link.c main.c sta_info.c scan.c util.c mlme.c tx.c rx.c wpa.c key.c \
         chan.c ht.c vht.c he.c wme.c agg-rx.c agg-tx.c driver-ops.h driver-ops.c \
         ieee80211_i.h sta_info.h; do
    fetch net/mac80211/$f third_party/linux-reference/net/mac80211/$f
done
