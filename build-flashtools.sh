#!/usr/bin/env bash
# Build the flash tooling for every device family that has LK sources: the
# LK images plus the flash CLI, one boot dir per variant. The developer
# picks a boot dir and flashes from it; this script never picks for them.
#
# This script builds nothing itself. It runs tools/mediatek/build.sh once per
# power mode -- battery, then without-battery -- which is what keeps it correct
# when the firmware gains flags: build.sh owns the power-mode table, this
# script only enumerates it. The device table lives here: j36-ultra is the
# proven LK (tools/mediatek/firmware, MT6592), and the mt67xx/mt68xx phone
# LKs (tools/mt67xx, tools/mt68xx) build behind their bring-up ACKs -- same
# command, same boot-dir-pick-and-flash flow, but the phone images are
# bring-up instruments that do not boot anything yet (see each tree's
# LK-BRINGUP.md). The remaining phones keep their stock bootloaders --
# OPPO's closed LK on the MT6877, Qualcomm aboot on the MSM8917 -- so there
# is nothing to build for them until vendor LK sources exist; --list states
# that per family instead of failing.
#
# Usage: ./build-flashtools.sh [--device NAME] [--battery-only | --without-battery] [--tests]
#
#   --device NAME      build only one family (j36-ultra, oppo-mt6877, lg-msm8917,
#                      oppo-mt6833, lg-mt6739). Families without LK sources fail
#                      loudly with the reason; the phone LK families need their
#                      bring-up ACK (OPPO_A77_BRINGUP_ACK / LG_K20_BRINGUP_ACK).
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
    printf '%-14s %s\n' oppo-mt6833 "builds: tools/mt68xx -> build/mt68xx/<device>/boot (bring-up LK; needs OPPO_A77_BRINGUP_ACK=1)"
    printf '%-14s %s\n' lg-mt6739 "builds: tools/mt67xx -> build/mt67xx/<device>/boot (bring-up LK; needs LG_K20_BRINGUP_ACK=1)"
}
# One phone LK family: every devices.sh row gets its own boot dir, and each
# dir is verified before the next builds -- a developer picks a boot dir
# as-is, so a silent shortfall here would ship as a broken flash there.
# Dies without the family's bring-up ACK: these images build but do not
# boot anything yet (see tools/<family>/LK-BRINGUP.md).
build_phone_lk() {
    local matrix="$1" family devices_fn ack_var dev rows dir f
    case "$matrix" in
        oppo-mt6833) family="mt68xx"; devices_fn="a77_devices"; ack_var="OPPO_A77_BRINGUP_ACK" ;;
        lg-mt6739) family="mt67xx"; devices_fn="k20_devices"; ack_var="LG_K20_BRINGUP_ACK" ;;
    esac
    if [[ "${!ack_var:-}" != 1 ]]; then
        echo "error: $matrix is bring-up scaffolding (see tools/$family/LK-BRINGUP.md): it builds but will not boot. Set $ack_var=1 to build anyway." >&2
        exit 1
    fi
    # shellcheck disable=SC1090
    . "$ROOT/device/oppo-a77/devices.sh"
    # shellcheck disable=SC1090
    . "$ROOT/device/lg-k20/devices.sh"
    rows="$($devices_fn)"
    for dev in $rows; do
        [[ -n "$dev" ]] || continue
        "$ROOT/tools/$family/build.sh" --device "$dev"
        dir="$ROOT/build/$family/$dev/boot"
        for f in lk.bin lk.elf FACTS.md build-info.txt; do
            [[ -s "$dir/$f" ]] || { echo "missing $dir/$f after build" >&2; exit 1; }
        done
    done
}

if [[ "$LIST_ONLY" == 1 ]]; then
    lk_matrix
    exit 0
fi
DO_J36=1
DO_PHONES=""
PHONE_TESTS=""
if [[ -n "$DEVICE" ]]; then
    case "$DEVICE" in
        j36-ultra) ;;
        oppo-mt6833|lg-mt6739)
            if [[ "$MODES" != "battery without-battery" ]]; then
                echo "error: --battery-only/--without-battery are j36-only; phone LKs are single-mode" >&2
                exit 2
            fi
            DO_J36=0
            DO_PHONES="$DEVICE"
            if [[ "$DEVICE" == oppo-mt6833 ]]; then PHONE_TESTS="mt68xx"; else PHONE_TESTS="mt67xx"; fi ;;
        oppo-mt6877|lg-msm8917)
            case "$DEVICE" in
                oppo-mt6877)
                    reason="the MT6877 LK is OPPO's closed bootloader"
                    instead="./build-oppo.sh" ;;
                lg-msm8917)
                    reason="the MSM8917 boots Qualcomm aboot, not LK"
                    instead="./build-lg.sh" ;;
            esac
            echo "error: no LK sources for $DEVICE -- $reason; the phone keeps its stock bootloader" >&2
            echo "Build its OS image instead: $instead." >&2
            echo "LK sources for it would start as a new tools/<soc>/ tree, not as modes here." >&2
            exit 1 ;;
        *) echo "Unknown device: $DEVICE (want j36-ultra, oppo-mt6877, lg-msm8917, oppo-mt6833, lg-mt6739)" >&2; exit 2 ;;
    esac
else
    DEVICE="j36-ultra"
    echo "NOTE: oppo-mt6877 and lg-msm8917 have no LK sources (stock bootloaders retained). See --list."
    # The phone LKs join the default run only behind their ACKs: their
    # images build but do not boot yet, and failing the default run on
    # their account would hold the proven j36 build hostage.
    if [[ "${OPPO_A77_BRINGUP_ACK:-}" == 1 ]]; then
        DO_PHONES="$DO_PHONES oppo-mt6833"
    else
        echo "NOTE: oppo-mt6833 skipped (bring-up LK; set OPPO_A77_BRINGUP_ACK=1 to build it)."
    fi
    if [[ "${LG_K20_BRINGUP_ACK:-}" == 1 ]]; then
        DO_PHONES="$DO_PHONES lg-mt6739"
    else
        echo "NOTE: lg-mt6739 skipped (bring-up LK; set LG_K20_BRINGUP_ACK=1 to build it)."
    fi
    PHONE_TESTS="mt67xx mt68xx"
fi

if [[ "$DO_J36" == 1 ]]; then
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
fi

# Phone LKs after the proven build, each verified inside build_phone_lk.
for matrix in $DO_PHONES; do
    [[ -n "$matrix" ]] || continue
    build_phone_lk "$matrix"
done
if [[ -n "${DO_PHONES// /}" ]]; then
    printf '\nPhone LK boot dirs built this run (bring-up instruments -- read the FACTS.md in each dir before flashing):\n'
    # shellcheck disable=SC1090
    . "$ROOT/device/oppo-a77/devices.sh"
    # shellcheck disable=SC1090
    . "$ROOT/device/lg-k20/devices.sh"
    for matrix in $DO_PHONES; do
        [[ -n "$matrix" ]] || continue
        case "$matrix" in
            oppo-mt6833) family="mt68xx"; rows="$(a77_devices)" ;;
            lg-mt6739) family="mt67xx"; rows="$(k20_devices)" ;;
        esac
        for dev in $rows; do
            [[ -n "$dev" ]] || continue
            printf '  build/%s/%s/boot/  (lk.bin -> LK/UBOOT slot)\n' "$family" "$dev"
        done
    done
fi

if [[ "$RUN_TESTS" == 1 ]]; then
    if [[ "$DO_J36" == 1 ]]; then
        # The build pins its own GOCACHE per mode; the test gets the shared
        # default so it never depends on $HOME being writable.
        export GOCACHE="${GOCACHE:-$ROOT/build/go-cache}"
        (cd "$ROOT/tools/mediatek/mvii-flash" && go test ./)
        cc -std=c99 -Wall -Wextra -Werror \
            "$ROOT/tools/mediatek/firmware/tests/test-bootmenu.c" \
            -o /tmp/j36-bootmenu-test && /tmp/j36-bootmenu-test
    fi
    for family in $PHONE_TESTS; do
        [[ -n "$family" ]] || continue
        cc -std=c99 -Wall -Wextra -Werror \
            "$ROOT/tools/$family/firmware/tests/test-lk-ui.c" \
            -o "/tmp/$family-lk-ui-test" && "/tmp/$family-lk-ui-test"
    done
fi
