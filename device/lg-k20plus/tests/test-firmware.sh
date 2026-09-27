#!/bin/sh
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# LG firmware test: the vendored prima set must exist, be non-empty, and
# match the hashes firmware/README.md promises -- a deleted or corrupted
# blob would otherwise break the build's firmware stage in the VM, far
# from the cause.
set -u

HERE="$(cd -- "$(dirname -- "$0")" && pwd)"
FW="$HERE/../firmware/wlan/prima"

if command -v shasum >/dev/null 2>&1; then
    sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
    sha256() { sha256sum "$1" | cut -d' ' -f1; }
fi

fail=0
want() { # $1 = file, $2 = size, $3 = sha256
    if [ ! -f "$FW/$1" ]; then
        echo "FAIL: $1 missing"; fail=1; return
    fi
    size="$(wc -c < "$FW/$1" | tr -d ' ')"
    if [ "$size" != "$2" ]; then
        echo "FAIL: $1 is $size bytes, want $2"; fail=1; return
    fi
    got="$(sha256 "$FW/$1")"
    if [ "$got" != "$3" ]; then
        echo "FAIL: $1 hash $got"; fail=1; return
    fi
    echo "  $1: ok ($size bytes)"
}

want WCNSS_qcom_wlan_nv.bin 29816 93fc87d8233ffb0244037ba165efdfcdc8851e4e7345fc646f5d628d29d703d8
want WCNSS_qcom_cfg.ini 9850 a8a748e831b510e3d60f8fb46796ec770e7546560aabc35432ce602b696f7583
want WCNSS_cfg.dat 11514 66d8aa043111f6bdce04cfb5c8d09d43f65f853506f996da357d89122e0ac1ae

[ "$fail" -eq 0 ] && echo "PASS: LG firmware"
exit "$fail"
