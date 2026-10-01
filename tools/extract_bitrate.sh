#!/bin/sh
# Rebuild src/compat_rtw89/rtw89_cfg80211_bitrate.c from a Linux
# net/wireless/util.c: the file's own header (everything up to the #include)
# plus the block from cfg80211_calculate_bitrate_ht() to the end of
# cfg80211_calculate_bitrate().
#
# Usage: tools/extract_bitrate.sh third_party/linux-reference/net/wireless/util.c
set -eu
src=$1
out=src/compat_rtw89/rtw89_cfg80211_bitrate.c
first=$(grep -n '^static u32 cfg80211_calculate_bitrate_ht' "$src" | cut -d: -f1)
last=$(grep -n '^EXPORT_SYMBOL(cfg80211_calculate_bitrate);' "$src" | cut -d: -f1)
head_end=$(grep -n '^#include "rtw89_net80211.h"' "$out" | cut -d: -f1)
tmp=$(mktemp)
{ sed -n "1,$((head_end + 1))p" "$out"; sed -n "${first},$((last - 1))p" "$src"; } > "$tmp"
mv "$tmp" "$out"
echo "$out: lines $first-$((last - 1)) of $src"
