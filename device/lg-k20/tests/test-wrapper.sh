#!/bin/sh
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# K20 wrapper test: the bring-up ACK gate must exist (this tree builds an
# image that will not boot until BRINGUP lands facts), the model dir must
# route through darkos_model_artifact_dir, and the VM/wrapper handover
# contract (full-image.txt keys) must agree on both sides.
set -u

ROOT="$(cd -- "$(dirname -- "$0")/../../.." && pwd)"
fail=0
if ! grep -q "LG_K20_BRINGUP_ACK" "$ROOT/build-lg-k20.sh"; then
    echo "FAIL: ACK gate missing"; fail=1
else
    echo "  ACK gate: ok"
fi
if ! grep -q 'darkos_model_artifact_dir "$BASE_ARTIFACT_DIR" lg ' "$ROOT/build-lg-k20.sh"; then
    echo "FAIL: model dir not routed"; fail=1
else
    echo "  model dir: ok"
fi
if ! grep -q "full-image.txt" "$ROOT/device/lg-k20/build-in-vm.sh"; then
    echo "FAIL: VM never writes full-image.txt"; fail=1
fi
for k in image boot_skip boot_count rootfs_skip rootfs_count; do
    grep -q "[[:space:]]$k)" "$ROOT/build-lg-k20.sh" \
        || { echo "FAIL: wrapper never reads handover key $k"; fail=1; }
done
echo "  handover contract: ok"
if ! "$ROOT/build-lg-k20.sh" --list-devices 2>/dev/null | grep -q lm-x120; then
    echo "FAIL: --list-devices broken"; fail=1
else
    echo "  --list-devices: ok"
fi
[ "$fail" -eq 0 ] && echo "PASS: K20 wrapper"
exit "$fail"
