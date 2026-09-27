#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# make-trixie-img.sh test: build a real ext4 image from a fixture rootfs and
# verify its label, size and contents with the e2fsprogs readers. Skips
# honestly when e2fsprogs is absent (stock macOS has no mke2fs).
set -u

HERE="$(cd -- "$(dirname -- "$0")" && pwd)"
SCRIPT="$HERE/../make-trixie-img.sh"

for d in /usr/local/opt/e2fsprogs/bin /usr/local/opt/e2fsprogs/sbin \
         /opt/homebrew/opt/e2fsprogs/bin /opt/homebrew/opt/e2fsprogs/sbin; do
    [[ -d "$d" ]] && PATH="$d:$PATH"
done
if ! command -v mke2fs >/dev/null || ! command -v dumpe2fs >/dev/null \
        || ! command -v debugfs >/dev/null; then
    echo "SKIP: make-trixie-img test needs e2fsprogs (mke2fs/dumpe2fs/debugfs)"
    exit 0
fi

fail=0
WORK="$(mktemp -d "${TMPDIR:-/tmp}/trixie-img-test.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/root/etc" "$WORK/root/sbin"
echo "trixie-test" > "$WORK/root/etc/hostname"
printf '#!/bin/sh\necho hi\n' > "$WORK/root/sbin/init"
chmod +x "$WORK/root/sbin/init"
ln -s sbin/init "$WORK/root/init-link"

"$SCRIPT" "$WORK/root" "$WORK/trixie.img" TESTFS || { echo "FAIL: builder exited nonzero"; exit 1; }
[[ -s "$WORK/trixie.img" ]] || { echo "FAIL: no image written"; exit 1; }

label="$(dumpe2fs -h "$WORK/trixie.img" 2>/dev/null | awk -F': ' '/volume name/{print $2}' | tr -d ' ')"
[[ "$label" == "TESTFS" ]] && echo "  label: ok ($label)" \
    || { echo "FAIL: label is '$label'"; fail=1; }

size="$(stat -f %z "$WORK/trixie.img" 2>/dev/null || stat -c %s "$WORK/trixie.img")"
blocks="$(dumpe2fs -h "$WORK/trixie.img" 2>/dev/null | awk '/^Block count/{print $3}')"
bsize="$(dumpe2fs -h "$WORK/trixie.img" 2>/dev/null | awk '/^Block size/{print $3}')"
[[ "$(( blocks * bsize ))" == "$size" ]] && echo "  size: ok ($size bytes)" \
    || { echo "FAIL: fs geometry ($blocks x $bsize) != file ($size)"; fail=1; }
[[ "$size" -ge $(( 256 * 1024 * 1024 )) ]] && echo "  minimum: ok" \
    || { echo "FAIL: tiny fixture should still yield >= 256 MiB, got $size"; fail=1; }

listing="$(debugfs -R "ls -l /" "$WORK/trixie.img" 2>/dev/null)"
for e in etc sbin init-link; do
    printf '%s\n' "$listing" | grep -q "$e" && echo "  entry $e: ok" \
        || { echo "FAIL: /$e missing from image"; fail=1; }
done
content="$(debugfs -R "cat etc/hostname" "$WORK/trixie.img" 2>/dev/null)"
[[ "$content" == "trixie-test" ]] && echo "  content: ok" \
    || { echo "FAIL: hostname reads '$content'"; fail=1; }
e2fsck -n -f "$WORK/trixie.img" >/dev/null 2>&1 && echo "  fsck: clean" \
    || { echo "FAIL: e2fsck unhappy"; fail=1; }

if "$SCRIPT" "$WORK/does-not-exist" "$WORK/no.img" >/dev/null 2>&1; then
    echo "FAIL: missing rootfs accepted"; fail=1
else
    echo "  missing rootfs: rejected ok"
fi

[ "$fail" -eq 0 ] && echo "PASS: make-trixie-img"
exit "$fail"
