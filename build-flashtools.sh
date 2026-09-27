#!/usr/bin/env bash
# Build the flash tooling for every device family that has LK sources: the
# LK images plus the flash CLI, one boot dir per variant. The developer
# picks a boot dir and flashes from it; this script never picks for them.
#
# This script builds nothing itself. It runs tools/mediatek/build.sh once per
# power mode -- battery, then without-battery -- which is what keeps it correct
# when the firmware gains flags: build.sh owns the power-mode table, this
# script only enumerates it. The device table lives here: j36-ultra is the
# only family with LK sources (tools/mediatek/firmware, MT6592). The phones
# keep their stock bootloaders -- OPPO's closed LK on the MT6877, Qualcomm
# aboot on the MSM8917 -- so there is nothing to build for them until
# vendor LK sources exist; --list states that per family instead of failing.
#
# Usage: ./build-flashtools.sh [--device NAME] [--battery-only | --without-battery] [--tests]
#
#   --device NAME      build only one family (j36-ultra, oppo-mt6877, lg-msm8917).
#                      Families without LK sources fail loudly with the reason.
#   --list             print the device matrix and exit without building
#   --battery-only     build only build/mediatek/j36-ultra/battery/boot
#   --without-battery  build only build/mediatek/j36-ultra/without-battery/boot
#   --tests            also run the flash-tool Go suite and the LK bootmenu C test
#
# After a build, flash from the matching boot dir, e.g.:
#   cd build/mediatek/j36-ultra/without-battery/boot
#   ./flash -root ./ -upload release -device /dev/cu.usbmodemXXXX -yes
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
MODES="battery without-battery"
DEVICE=""
RUN_TESTS=0
LIST_ONLY=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --device) [[ $# -ge 2 ]] || { echo '--device needs a name' >&2; exit 2; }; DEVICE="$2"; shift 2 ;;
        --list) LIST_ONLY=1; shift ;;
        --battery-only) MODES="battery"; shift ;;
        --without-battery) MODES="without-battery"; shift ;;
        --tests) RUN_TESTS=1; shift ;;
        -h|--help) sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d'; exit 0 ;;
        *) echo "Unknown option: $1 (see --help)" >&2; exit 2 ;;
    esac
done

lk_matrix() {
    printf '%-14s %s\n' j36-ultra "builds: tools/mediatek/firmware -> build/mediatek/j36-ultra/<mode>/boot"
    printf '%-14s %s\n' oppo-mt6877 "no LK sources: the Dimensity 900 LK is OPPO's closed bootloader; the phone keeps stock LK (OS image: ./build-oppo.sh)"
    printf '%-14s %s\n' lg-msm8917 "no LK sources: the MSM8917 boots Qualcomm aboot, not LK; the phone keeps stock aboot (OS image: ./build-lg.sh)"
}
if [[ "$LIST_ONLY" == 1 ]]; then
    lk_matrix
    exit 0
fi
if [[ -n "$DEVICE" ]]; then
    case "$DEVICE" in
        j36-ultra) ;;
        oppo-mt6877|lg-msm8917)
            if [[ "$DEVICE" == oppo-mt6877 ]]; then
                reason="the MT6877 LK is OPPO's closed bootloader"
                instead="./build-oppo.sh"
            else
                reason="the MSM8917 boots Qualcomm aboot, not LK"
                instead="./build-lg.sh"
            fi
            echo "error: no LK sources for $DEVICE -- $reason; the phone keeps its stock bootloader" >&2
            echo "Build its OS image instead: $instead." >&2
            echo "LK sources for it would start as a new tools/<soc>/ tree, not as modes here." >&2
            exit 1 ;;
        *) echo "Unknown device: $DEVICE (want j36-ultra, oppo-mt6877, lg-msm8917)" >&2; exit 2 ;;
    esac
else
    DEVICE="j36-ultra"
    echo "NOTE: oppo-mt6877 and lg-msm8917 have no LK sources (stock bootloaders retained); building j36-ultra only. See --list."
fi

export GOFLAGS="${GOFLAGS:--mod=mod}"
for mode in $MODES; do
    if [[ "$mode" == "without-battery" ]]; then
        "$ROOT/tools/mediatek/build.sh" --without-battery
    else
        "$ROOT/tools/mediatek/build.sh"
    fi
done

# Every variant verified: the five artifacts tools/mediatek/build.sh
# promises, in each mode's boot dir. A developer picks a boot dir as-is,
# so a silent shortfall here would ship as a broken flash there.
for mode in $MODES; do
    dir="$ROOT/build/mediatek/j36-ultra/$mode/boot"
    for f in lk.bin lk-release.bin MVIIFlash.bin assets.bin flash; do
        [[ -s "$dir/$f" ]] || { echo "missing $dir/$f after build" >&2; exit 1; }
    done
done
printf '\nAll LK variants are built; pick a boot dir and flash from it:\n'
for mode in $MODES; do
    printf '  build/mediatek/j36-ultra/%s/boot/  (lk-release.bin -> LK/UBOOT slot)\n' "$mode"
done

if [[ "$RUN_TESTS" == 1 ]]; then
    # The build pins its own GOCACHE per mode; the test gets the shared
    # default so it never depends on $HOME being writable.
    export GOCACHE="${GOCACHE:-$ROOT/build/go-cache}"
    (cd "$ROOT/tools/mediatek/mvii-flash" && go test ./)
    cc -std=c99 -Wall -Wextra -Werror \
        "$ROOT/tools/mediatek/firmware/tests/test-bootmenu.c" \
        -o /tmp/j36-bootmenu-test && /tmp/j36-bootmenu-test
fi
