#!/bin/bash
# Prove the boot partition before the image is archived.
#
# This runs after clean_mounts.sh, so the partition is unmounted and everything
# written to it has been flushed into $DISK: the check reads the same bytes a
# card reader would.  A build that gets here with an unbootable BOOT partition
# has to fail, because the alternative is what happened with the 08042026 image
# -- a 2.3 GB archive that verifies, uploads and boots to nothing.

boot_log() { printf '\n[r36-boot] %s\n' "$*"; }

VERIFY_BOOT="device/r36-ultra/verify_boot.py"
[[ -f "$VERIFY_BOOT" ]] || { printf '\n[r36-boot] ERROR: %s is missing\n' "$VERIFY_BOOT" >&2; exit 1; }

boot_log "Verifying the boot partition of $DISK"
if ! python3 "$VERIFY_BOOT" "$DISK" \
        --require Image \
        --require uInitrd \
        --require boot.ini \
        --require "${BOOT_KERNEL_DTB:-$KERNEL_DTB}"; then
    printf '\n[r36-boot] ERROR: %s has no bootable BOOT partition; refusing to archive it\n' \
        "$DISK" >&2
    exit 1
fi
boot_log "Boot partition verified"
