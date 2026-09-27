#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# J36-only BOOT test: a full ./build-j36-ultra.sh run must not mix the R36S
# boot set into the card image, and --mix-only must stage the J36 launcher
# only. The image's p1 is emptied before the launcher goes in (the base image
# itself is untouched -- the injection runs on a copy), so Image, uInitrd,
# rk3326/rg351mp trees, boot.ini and the R36S helpers never reach a J36 card.
# boot.conf sits at the BOOT root; the LK reads it there first and falls back
# to the legacy mvii/ path for cards written by older builds.
# Static: greps the scripts, so it runs on the workstation with no VM.
set -u

HERE="$(cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(cd -- "$HERE/../../.." && pwd)"
INVM="$ROOT/device/j36-ultra/build-in-vm.sh"
README="$ROOT/device/j36-ultra/README.md"
LKMAIN="$ROOT/tools/mediatek/mt65xx/firmware/Drivers/mvii_lk_main.c"

fail=0

# 1. p1 is emptied before the launcher copy lands on it.
wipe_line="$(grep -n -F 'find "$mnt" -mindepth 1 -delete' "$INVM" | cut -d: -f1 || true)"
copy_line="$(grep -n -F 'cp -r "$SDBOOT/." "$mnt/"' "$INVM" | cut -d: -f1 || true)"
if [[ -z "$wipe_line" ]]; then
    echo "FAIL: no p1 clean-out (find \$mnt -mindepth 1 -delete) in inject_into_image"
    fail=1
elif [[ -z "$copy_line" ]]; then
    echo "FAIL: p1 launcher copy went missing from inject_into_image"
    fail=1
elif [[ "$wipe_line" -gt "$copy_line" ]]; then
    echo "FAIL: p1 is emptied after the launcher copy (wipe=$wipe_line copy=$copy_line)"
    fail=1
else
    echo "  p1 emptied before the launcher copy: ok (wipe=$wipe_line copy=$copy_line)"
fi

# 2. The old contract -- R36S files "stay there" on p1 -- is gone.
if grep -q -F "are on this partition and stay there" "$INVM"; then
    echo "FAIL: stale added-to-never-replaced contract still in $INVM"
    fail=1
else
    echo "  no stay-there contract: ok"
fi

# 3. The staged README no longer promises coexistence on BOOT.
if grep -q -F "Existing files are not disturbed" "$INVM"; then
    echo "FAIL: staged README.txt still promises R36S coexistence"
    fail=1
else
    echo "  staged README promises J36-only: ok"
fi
if grep -q -F "an R36S card shares with its own boot files" "$INVM"; then
    echo "FAIL: staged README/comment still describes a shared BOOT partition"
    fail=1
else
    echo "  no shared-partition wording: ok"
fi

# 4. The device README matches the build.
if grep -q -F 'keeps its `Image`' "$README"; then
    echo "FAIL: device README still promises R36S coexistence on BOOT"
    fail=1
else
    echo "  device README promises J36-only: ok"
fi

# 5. The image signature invalidates outputs written before the clean-out,
# so the first run after this change re-injects rather than reusing one.
if grep -q -F "p1 j36-only v1" "$INVM"; then
    echo "  image signature carries the p1 marker: ok"
else
    echo "FAIL: image_export_signature has no p1 j36-only marker; pre-change outputs would be reused"
    fail=1
fi

# 6. Guards: the J36 set is still staged, and no R36S name is staged into it.
staged_ok=0
for member in 'cp "$ZIMAGE" "$SDBOOT/zImage"' \
        'cp "$DTB_OUT/mt6592-j36-ultra.dtb" "$SDBOOT/"' \
        'cp "$ARTIFACTS/initramfs-j36-ultra.cpio.xz" "$SDBOOT/initrd.img"' \
        'cat > "$SDBOOT/boot.conf"' \
        'cat > "$SDBOOT/README.txt"' \
        'cat > "$SDBOOT/LICENSE.txt"' \
        'cp "$ARTIFACTS/sd-root.tar.gz" "$SDBOOT/sd-root.tar.gz"'; do
    if grep -q -F "$member" "$INVM"; then
        staged_ok=$((staged_ok + 1))
    else
        echo "FAIL: J36 staging lost: $member"
        fail=1
    fi
done
[[ "$staged_ok" == 7 ]] && echo "  J36 staging intact (7 members): ok"
if grep -E -q '"\$SDBOOT/([^"]*/)?(Image|uInitrd|boot\.ini|logo\.bmp|rk3326[^"/]*|rg351mp[^"/]*|firstboot\.sh|expandtoexfat[^"/]*|fstab\.exfat[^"/]*)"' "$INVM"; then
    echo "FAIL: an R36S-named file is staged into \$SDBOOT:"
    grep -E -n '"\$SDBOOT/([^"]*/)?(Image|uInitrd|boot\.ini|logo\.bmp|rk3326[^"/]*|rg351mp[^"/]*|firstboot\.sh|expandtoexfat[^"/]*|fstab\.exfat[^"/]*)"' "$INVM"
    fail=1
else
    echo "  no R36S names staged into SDBOOT: ok"
fi

# 7. The LK reads boot.conf at the root first, mvii/ as the legacy fallback.
if grep -q -F '#define SD_CONF_PATH "/boot.conf"' "$LKMAIN" \
    && grep -q -F '#define SD_CONF_FALLBACK "/mvii/boot.conf"' "$LKMAIN"; then
    echo "  LK conf paths (root + legacy fallback): ok"
else
    echo "FAIL: LK does not read /boot.conf with an /mvii/boot.conf fallback"
    fail=1
fi
root_line="$(grep -n -F 'mvii_fat_read_file(&g_sd_fs, SD_CONF_PATH' "$LKMAIN" | cut -d: -f1 || true)"
fb_line="$(grep -n -F 'mvii_fat_read_file(&g_sd_fs, SD_CONF_FALLBACK' "$LKMAIN" | cut -d: -f1 || true)"
if [[ -z "$root_line" || -z "$fb_line" ]]; then
    echo "FAIL: LK does not try both conf paths in lk_sd_boot"
    fail=1
elif [[ "$root_line" -gt "$fb_line" ]]; then
    echo "FAIL: LK tries the legacy mvii/ path before /boot.conf (root=$root_line fallback=$fb_line)"
    fail=1
else
    echo "  LK tries root before legacy: ok (root=$root_line fallback=$fb_line)"
fi

# 8. No stale shared-partition wording survives anywhere in the J36 notes.
if grep -q -F "shared with an R36S card's own boot files" "$INVM"; then
    echo "FAIL: stale shared-partition wording still in $INVM"
    fail=1
else
    echo "  no stale shared-partition wording: ok"
fi

# 9. A bare mvii/ directory never identifies BOOT: the only legacy marker is
# the mvii/boot.conf file, -f tested at the three identification sites
# (mount_bootfs, the splash-tick trail, j36-logdump).
if grep -E -q '\[ -d [^]]*mvii' "$INVM"; then
    echo "FAIL: a bare mvii/ directory still identifies BOOT:"
    grep -E -n '\[ -d [^]]*mvii' "$INVM"
    fail=1
else
    echo "  no bare-mvii/ directory check: ok"
fi
legacy_code="$(grep -n -F "mvii/boot.conf" "$INVM" | grep -v -E ':[[:space:]]*#' | grep -v -E 'say |echo ' || true)"
legacy_bad="$(printf '%s\n' "$legacy_code" | grep -v -F '[ -f ' || true)"
legacy_n="$(printf '%s\n' "$legacy_code" | grep -c -F '[ -f ' || true)"
if [ -n "$legacy_bad" ]; then
    echo "FAIL: mvii/boot.conf used outside a -f file test:"
    printf '%s\n' "$legacy_bad"
    fail=1
elif [ "$legacy_n" != 3 ]; then
    echo "FAIL: want the legacy file tested at exactly 3 sites, found $legacy_n"
    fail=1
else
    echo "  legacy file -f tested at 3 sites: ok"
fi

[ "$fail" -eq 0 ] && echo "PASS: J36-only BOOT"
exit "$fail"
