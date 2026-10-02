# Sourced by apply.sh and revert.sh: mount the EFI partition that holds
# OpenCore and set EFI_MOUNT. With an argument, only that partition is tried.
find_efi() {
    EFI_MOUNT=
    if [ -n "$1" ]; then
        candidates="$1"
    else
        candidates="$(diskutil list | awk '$2 == "EFI" { print $NF }')"
    fi
    for part in $candidates; do
        diskutil mount "$part" >/dev/null 2>&1 || continue
        mp="$(diskutil info "$part" | sed -n 's/^ *Mount Point: *//p')"
        if [ -n "$mp" ] && [ -f "$mp/EFI/OC/config.plist" ]; then
            EFI_MOUNT="$mp"
            return 0
        fi
    done
    echo "no EFI partition with EFI/OC/config.plist found; pass it, e.g. sudo $0 disk1s1" >&2
    exit 1
}
