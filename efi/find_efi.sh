# Sourced by apply.sh and revert.sh: find the volume that holds OpenCore and
# set EFI_MOUNT. The argument, if any, is a partition (disk3s1) or the path of
# a mounted volume; without one every EFI and FAT partition is tried, since
# OpenCore may live on a USB stick rather than on the internal EFI partition.
find_efi() {
    EFI_MOUNT=
    if [ -n "$1" ] && [ -d "$1" ]; then
        [ -f "$1/EFI/OC/config.plist" ] || { echo "no EFI/OC/config.plist under $1" >&2; exit 1; }
        EFI_MOUNT="$1"
        return 0
    fi
    if [ -n "$1" ]; then
        candidates="$1"
    else
        candidates="$(diskutil list | awk '$2 == "EFI" || $2 ~ /FAT/ { print $NF }')"
    fi
    for part in $candidates; do
        # a stick that does not answer must not hang the script
        perl -e 'alarm 90; exec @ARGV' diskutil mount "$part" >/dev/null 2>&1 || continue
        mp="$(perl -e 'alarm 30; exec @ARGV' diskutil info "$part" | sed -n 's/^ *Mount Point: *//p')"
        if [ -n "$mp" ] && [ -f "$mp/EFI/OC/config.plist" ]; then
            EFI_MOUNT="$mp"
            return 0
        fi
    done
    echo "no volume with EFI/OC/config.plist found among: $(echo $candidates)" >&2
    echo "mount the one OpenCore boots from and pass it: sudo $0 disk3s1   or   sudo $0 /Volumes/NAME" >&2
    exit 1
}
