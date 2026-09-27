#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# Artifact layout test: every family wrapper must land its deliverables in
# MixOS-Artifacts/<family>/<model>/ through darkos_model_artifact_dir, so
# no two builds share a filename. Tests the helper's output, then the
# wiring (each wrapper calls it with its own family).
set -u

HERE="$(cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(cd -- "$HERE/../../.." && pwd)"
# shellcheck source=device/common/multipass.sh
. "$HERE/../multipass.sh"

fail=0
expect() {
    local got="$1" want="$2" what="$3"
    if [[ "$got" == "$want" ]]; then
        echo "  $what: ok ($got)"
    else
        echo "FAIL: $what: got '$got', want '$want'"
        fail=1
    fi
}

expect "$(darkos_model_artifact_dir /base oppo 20181)" "/base/oppo/20181" "oppo model dir"
expect "$(darkos_model_artifact_dir /base lg lv517)" "/base/lg/lv517" "lg model dir"
expect "$(darkos_model_artifact_dir /base qbuy j36-ultra)" "/base/qbuy/j36-ultra" "qbuy j36 dir"
expect "$(darkos_model_artifact_dir /base qbuy r36-ultra)" "/base/qbuy/r36-ultra" "qbuy r36 dir"

wired() { # $1 = wrapper file, $2 = family
    if grep -q "darkos_model_artifact_dir \"\$BASE_ARTIFACT_DIR\" $2 " "$ROOT/$1"; then
        echo "  $1: wired to $2 ok"
    else
        echo "FAIL: $1 does not route ARTIFACT_DIR through darkos_model_artifact_dir $2"
        fail=1
    fi
}

wired build-oppo.sh oppo
wired build-lg.sh lg
wired build-oppo-a77.sh oppo
wired build-lg-k20.sh lg
wired build-j36-ultra.sh qbuy
wired build-r36-ultra.sh qbuy

# The VM half and the wrapper half must agree on the handover contract:
# the VM writes full-image.txt (image= plus make_full_img.py's offsets) and
# the wrapper reads that same file back out of the VM work dir.
handover_keys() { # $1 = device dir, $2 = wrapper, $3 = family
    local dir=$1 wrapper=$2 family=$3 k
    grep -q "full-image.txt" "$ROOT/$dir/build-in-vm.sh" \
        || { echo "FAIL: $dir/build-in-vm.sh never writes full-image.txt"; fail=1; }
    grep -q "make_full_img.py" "$ROOT/$dir/build-in-vm.sh" \
        || { echo "FAIL: $dir/build-in-vm.sh never assembles the full image"; fail=1; }
    grep -q "full-image.txt" "$ROOT/$wrapper" \
        || { echo "FAIL: $wrapper never reads full-image.txt"; fail=1; }
    for k in image boot_skip boot_count rootfs_skip rootfs_count; do
        grep -q "[[:space:]]$k)" "$ROOT/$wrapper" \
            || { echo "FAIL: $wrapper never reads handover key $k"; fail=1; }
    done
    echo "  $family handover contract: ok"
}

handover_keys device/oppo-mt6877 build-oppo.sh oppo
handover_keys device/lg-k20plus build-lg.sh lg
handover_keys device/oppo-a77 build-oppo-a77.sh oppo-a77
handover_keys device/lg-k20 build-lg-k20.sh lg-k20

# Offline firmware stays wired: each shipped wrapper must honor its ROM
# env knob by invoking its fetch script into the default stock/ dir.
# (The bring-up scaffolds take a staged FIRMWARE_DIR only; their fetch
# wiring lands at BRINGUP step 4, so they have no rows here yet.)
firmware_wired() { # $1 = wrapper, $2 = env knob, $3 = fetch script
    grep -q "$2" "$ROOT/$1" \
        || { echo "FAIL: $1 ignores $2"; fail=1; }
    grep -q "$3" "$ROOT/$1" \
        || { echo "FAIL: $1 never invokes $3"; fail=1; }
    echo "  $2 -> $3: ok"
}

firmware_wired build-oppo.sh OPPO_OFP firmware/fetch-ofp.sh
firmware_wired build-lg.sh LG_KDZ firmware/fetch-kdz.sh

[ "$fail" -eq 0 ] && echo "PASS: artifact layout"
exit "$fail"
