# Sourced by the install scripts: mount the internal EFI partition that holds
# the second OpenCore copy and set INTERNAL_OC to its EFI/OC folder.
#
# Found by its partition UUID, not by a disk number: those change from one
# boot to the next (on 2026-10-02 the partition was disk1s1, later disk0s1,
# and disk1s1 had become a Windows dynamic disk's metadata partition).
INTERNAL_EFI_UUID=BC5C5FBD-D3C3-42BD-AE73-558AE9D9D5A5

internal_efi() {
    info="$(perl -e 'alarm 30; exec @ARGV' diskutil info "$INTERNAL_EFI_UUID" 2>/dev/null)" || {
        echo "the internal EFI partition ($INTERNAL_EFI_UUID) is not there" >&2; exit 1; }
    part="$(printf '%s\n' "$info" | sed -n 's/^ *Device Identifier: *//p')"
    fs="$(printf '%s\n' "$info" | sed -n 's/^ *File System Personality: *//p')"
    case "$fs" in
        *FAT32*) ;;
        *) echo "$part ($INTERNAL_EFI_UUID) is not FAT32 ($fs): not touching it" >&2; exit 1 ;;
    esac
    perl -e 'alarm 90; exec @ARGV' diskutil mount "$part" >/dev/null || {
        echo "could not mount $part" >&2; exit 1; }
    mp="$(diskutil info "$part" | sed -n 's/^ *Mount Point: *//p')"
    INTERNAL_OC="$mp/EFI/OC"
    [ -f "$INTERNAL_OC/config.plist" ] || {
        echo "no OpenCore at $INTERNAL_OC (see efi/install_internal.sh)" >&2; exit 1; }
    echo "internal EFI: $part, $mp"
}
