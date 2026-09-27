#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# J36-only BOOT test: a full ./build-j36-ultra.sh run must not mix the R36S
# boot set into the card image, and --mix-only must stage the J36 launcher
# only. The image's p1 is emptied before the launcher goes in (the base image
# itself is untouched -- the injection runs on a copy), so Image, uInitrd,
# rk3326/rg351mp trees, boot.ini and the R36S helpers never reach a J36 card.
# Static: greps the in-VM script, so it runs on the workstation with no VM.
set -u

HERE="$(cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(cd -- "$HERE/../../.." && pwd)"
INVM="$ROOT/device/j36-ultra/build-in-vm.sh"
README="$ROOT/device/j36-ultra/README.md"

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
        'cat > "$SDBOOT/mvii/boot.conf"' \
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

[ "$fail" -eq 0 ] && echo "PASS: J36-only BOOT"
exit "$fail"
