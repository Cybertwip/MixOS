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
wired build-j36-ultra.sh qbuy
wired build-r36-ultra.sh qbuy

# The VM half and the wrapper half must agree on the manifest contract:
# every key the wrapper reads is written by the VM script, under the same
# manifest filename on both sides.
manifest_keys() { # $1 = device dir, $2 = wrapper, $3 = file prefix
    local dir=$1 wrapper=$2 prefix=$3 k
    for k in bootimg trixieimg rootfs; do
        grep -q "echo \"$k=" "$ROOT/$dir/build-in-vm.sh" \
            || { echo "FAIL: $dir/build-in-vm.sh never writes manifest key $k"; fail=1; }
        grep -q "[[:space:]]$k)" "$ROOT/$wrapper" \
            || { echo "FAIL: $wrapper never reads manifest key $k"; fail=1; }
    done
    grep -q "$prefix-\\\$DEVICE-manifest.txt" "$ROOT/$dir/build-in-vm.sh" \
        || { echo "FAIL: $dir/build-in-vm.sh manifest filename drifted"; fail=1; }
    grep -q "$prefix-\\\$DEVICE-manifest.txt" "$ROOT/$wrapper" \
        || { echo "FAIL: $wrapper manifest filename drifted"; fail=1; }
    echo "  $prefix manifest contract: ok"
}

manifest_keys device/oppo-mt6877 build-oppo.sh oppo
manifest_keys device/lg-k20plus build-lg.sh lg

[ "$fail" -eq 0 ] && echo "PASS: artifact layout"
exit "$fail"
