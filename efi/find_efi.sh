# Sourced by the EFI scripts: find the volume that holds OpenCore and set
# EFI_MOUNT (its mount point) and EFI_PART (its device, e.g. disk0s1).
#
# The argument, if any, picks it: a partition (disk0s1), a partition UUID (as
# diskutil info shows it) or the path of a mounted volume. Without one, every
# EFI and FAT partition is mounted and looked at; if exactly one has
# EFI/OC/config.plist that is the one, if several do (an internal copy and a
# USB stick, say) the script stops and lists them so the person can choose.
# Disk numbers change from boot to boot: a UUID is the safe way to name one.
find_efi() {
    EFI_MOUNT=
    EFI_PART=
    if [ -n "$1" ] && [ -d "$1" ]; then
        [ -f "$1/EFI/OC/config.plist" ] || { echo "no EFI/OC/config.plist under $1" >&2; exit 1; }
        EFI_MOUNT="$1"
        EFI_PART="$(diskutil info "$1" 2>/dev/null | sed -n 's/^ *Device Identifier: *//p')"
        return 0
    fi
    if [ -n "$1" ]; then
        candidates="$1"
    else
        candidates="$(diskutil list | awk '$2 == "EFI" || $2 ~ /FAT/ { print $NF }')"
    fi
    found=
    for part in $candidates; do
        info="$(perl -e 'alarm 30; exec @ARGV' diskutil info "$part" 2>/dev/null)" || continue
        dev="$(printf '%s\n' "$info" | sed -n 's/^ *Device Identifier: *//p')"
        fs="$(printf '%s\n' "$info" | sed -n 's/^ *File System Personality: *//p')"
        case "$fs" in
            *FAT*|*fat*|*MS-DOS*) ;;
            *) [ -n "$1" ] && { echo "$part is not a FAT volume ($fs): not touching it" >&2; exit 1; }
               continue ;;
        esac
        # a stick that does not answer must not hang the script
        perl -e 'alarm 90; exec @ARGV' diskutil mount "$dev" >/dev/null 2>&1 || continue
        mp="$(perl -e 'alarm 30; exec @ARGV' diskutil info "$dev" | sed -n 's/^ *Mount Point: *//p')"
        if [ -n "$mp" ] && [ -f "$mp/EFI/OC/config.plist" ]; then
            uuid="$(diskutil info "$dev" | sed -n 's/^ *Disk \/ Partition UUID: *//p')"
            found="$found$dev|$mp|$uuid
"
        fi
    done
    count="$(printf '%s' "$found" | grep -c . || true)"
    if [ "$count" -eq 1 ]; then
        EFI_PART="$(printf '%s' "$found" | cut -d'|' -f1)"
        EFI_MOUNT="$(printf '%s' "$found" | cut -d'|' -f2)"
        return 0
    fi
    if [ "$count" -eq 0 ]; then
        echo "no volume with EFI/OC/config.plist found among: $(echo $candidates)" >&2
        echo "mount the one OpenCore boots from and pass it: sudo $0 /Volumes/NAME" >&2
    else
        echo "OpenCore is on more than one volume; say which one this machine boots from:" >&2
        printf '%s' "$found" | while IFS='|' read -r dev mp uuid; do
            echo "  $dev  mounted at $mp  (UUID $uuid)" >&2
        done
        echo "for example: sudo $0 <UUID>   (UUIDs stay the same across boots; disk numbers do not)" >&2
    fi
    exit 1
}

# efi_room KEXT...: stop, before anything is changed, unless the EFI volume
# ($EFI_MOUNT, with OpenCore's Kexts in $oc/Kexts) can take these kexts in
# place of the copies of them already there, plus $EFI_EXTRA_KB and 1 MB to
# spare. The driver carries every chip's firmware, about 12 MB.
efi_room() {
    need=$((${EFI_EXTRA_KB:-0} + 1024))
    for k; do
        name="$(basename "$k")"
        need=$((need + $(du -sk "$k" | cut -f1)))
        [ -d "$oc/Kexts/$name" ] && need=$((need - $(du -sk "$oc/Kexts/$name" | cut -f1)))
    done
    free="$(df -k "$EFI_MOUNT" | awk 'NR == 2 { print $4 }')"
    if [ "$free" -lt "$need" ]; then
        echo "the EFI volume has $free KB free and this needs $need KB. Nothing was changed." >&2
        echo "make room on it (old kexts or other operating systems' files you no longer boot) and run this again." >&2
        exit 1
    fi
}
